#include "regional_flag_surfaces_probe.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/composer_autocomplete.hpp"
#include "maicord/ui/message_reaction_presentation.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/reaction_viewer_presentation.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/media/regional_flag_svg.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/reaction_viewer.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "colr_binding_probe.hpp"
#include "regional_flag_upload_probe.hpp"
#include "probe_present.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
namespace {
constexpr Utf8View tj=utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF");
constexpr Utf8View tk=utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xB0");
constexpr u16 sequences[]{504,505};
constexpr Utf8View text=utf8_literal(
    "Plain \xF0\x9F\x87\xB9\xF0\x9F\x87\xAF and \xF0\x9F\x87\xB9\xF0\x9F\x87\xB0\n"
    "Code `\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF` and hidden ||\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF||\n"
    "Bidi \xD7\x90\xD7\x91 \xF0\x9F\x87\xB9\xF0\x9F\x87\xAF \xD8\xA7\xD8\xA8 \xF0\x9F\x87\xB9\xF0\x9F\x87\xB0\n"
    "Wrap WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW "
    "\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF end");
constexpr Utf8View draft=utf8_literal("a\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF" "b\xF0\x9F\x87\xB9\xF0\x9F\x87\xB0" "c");

bool fail(u32 line) noexcept {
    log_u64(LogLevel::error,"probe","regional_flag_surfaces_failure_line",line);return false;
}
bool within(float a,float b) noexcept {return a-b<.25f && b-a<.25f;}
void dip(const char* name,float value) noexcept {
    log_u64(LogLevel::error,"probe",name,static_cast<u64>(maximum(0.f,value)*1000.f+.5f));
}
void diagnostic_rectangle(UiRect rectangle) noexcept {
    dip("regional_flag_surfaces_rect_left_millidip",rectangle.left);
    dip("regional_flag_surfaces_rect_top_millidip",rectangle.top);
    dip("regional_flag_surfaces_rect_right_millidip",rectangle.right);
    dip("regional_flag_surfaces_rect_bottom_millidip",rectangle.bottom);
}
bool diagnostic(const char* reason,u32 index=0) noexcept {
    log_write(LogLevel::error,"probe",reason);
    log_u64(LogLevel::error,"probe","regional_flag_surfaces_detail_index",index);return false;
}
void font_diagnostic(const D3D11Renderer& renderer) noexcept {
    const auto& system=renderer.glyph_text.system;
    dip("regional_flag_surfaces_renderer_em_millidip",system.em_size);
    log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_native_quad_count",renderer.content_text->quad_count);
    log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_hit_region_count",renderer.content_text->content_hit_region_count);
    log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_media_quad_count",renderer.media_quad_count);
}
bool same_handle(MediaAtlasHandle a,MediaAtlasHandle b) noexcept {
    return a.slot==b.slot && a.generation==b.generation;
}
// Read exactly the fixed metadata target from the supplied offline corpus.
bool upload_flags(D3D11Renderer& renderer) noexcept {
    wchar_t directory[1000]{};
    const u32 chars=GetEnvironmentVariableW(L"MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY",directory,1000);
    if (!chars || chars>=1000 || !renderer.media_atlas) return false;
    Arena scratch{};struct Release {Arena& arena;~Release() noexcept {arena_release(&arena);}} release{scratch};
    if (!arena_initialize(&scratch,kRegionalFlagSvgMaximumBytes,0,4096)) return false;
    auto* encoded=arena_allocate<u8>(&scratch,kRegionalFlagSvgMaximumBytes);
    if (!encoded) return false;
    for (auto sequence:sequences) {
        char target[kRegionalFlagSvgTargetBytes]{};wchar_t path[1024]{};
        const u32 target_bytes=regional_flag_svg_target(sequence,{target,sizeof(target)});
        if (!target_bytes || chars > 977) return false;
        copy_bytes(path,directory,chars*sizeof(wchar_t));u32 used=chars;path[used++]=L'\\';
        for (u32 i=kRegionalFlagSvgTargetPrefixBytes;i<target_bytes;++i) path[used++]=static_cast<wchar_t>(target[i]);
        HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE) return false;
        Win32LargeInteger size{};DWORD read=0;
        const bool valid=GetFileSizeEx(file,size.native()) && size.QuadPart>0 && size.QuadPart<=kRegionalFlagSvgMaximumBytes &&
            ReadFile(file,encoded,static_cast<DWORD>(size.QuadPart),&read,nullptr) && read==static_cast<DWORD>(size.QuadPart);
        const bool closed=CloseHandle(file)!=FALSE;
        if (!valid || !closed) return false;
        const auto uploaded=probe_upload_regional_flag(&renderer,media_asset_regional_flag(sequence),{encoded,read});
        if (!uploaded.ok() || uploaded.allocation.width!=64 || uploaded.allocation.height!=64 ||
            !colr_compute_bindings_empty(renderer.graphics.context)) return false;
    }
    return true;
}
bool paint(RegionalFlagSurfacesProbeContext& c) noexcept {
    return c.rebuild() && probe_present(&c.renderer,kDefaultTheme);
}
u32 flag_quads(const D3D11Renderer& renderer) noexcept {
    u32 count=0;
    for (auto sequence:sequences) {
        const auto allocation=media_atlas_peek(renderer.media_atlas,media_asset_regional_flag(sequence));
        if (!allocation.ok()) return ~0u;
        for (u32 i=0;i<renderer.media_quad_count;++i)
            count+=renderer.media_draw.domain_data[i]==0 && same_handle(renderer.media_draw.handle_data[i],allocation.handle);
    }
    return count;
}
bool quad_at(const D3D11Renderer& renderer,u16 sequence,UiRect rectangle,UiRect clip) noexcept {
    const auto key=media_asset_regional_flag(sequence);
    const auto allocation=media_atlas_peek(renderer.media_atlas,key);
    if (!allocation.ok() || key.kind!=MediaAssetKind::regional_flag || key.id!=sequence || key.variant || key.flags ||
        !ui_rect_has_area(ui_rect_intersection(rectangle,clip))) return false;
    for (u32 i=0;i<renderer.media_quad_count;++i) {
        const auto& q=renderer.media_draw.quad_data[i];
        if (renderer.media_draw.domain_data[i] || !same_handle(renderer.media_draw.handle_data[i],allocation.handle) ||
            !within(q.left,rectangle.left) || !within(q.top,rectangle.top) ||
            !within(q.right,rectangle.right) || !within(q.bottom,rectangle.bottom)) continue;
        for (u32 j=0;j<renderer.media_batch_count;++j) {
            const auto& batch=renderer.media_draw.batch_data[j];
            if (i>=batch.first_quad && i<batch.first_quad+batch.quad_count && ui_rect_equal(batch.clip,clip)) return true;
        }
    }
    log_u64(LogLevel::error,"probe","regional_flag_surfaces_expected_quad_sequence",sequence);
    log_write(LogLevel::error,"probe","regional_flag_surfaces_expected_quad_rectangle");diagnostic_rectangle(rectangle);
    log_write(LogLevel::error,"probe","regional_flag_surfaces_expected_quad_scissor");diagnostic_rectangle(clip);
    for (u32 i=0;i<renderer.media_quad_count;++i) {
        if (renderer.media_draw.domain_data[i] || !same_handle(renderer.media_draw.handle_data[i],allocation.handle)) continue;
        const auto& q=renderer.media_draw.quad_data[i];
        log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_matching_quad_index",i);
        log_write(LogLevel::error,"probe","regional_flag_surfaces_actual_matching_quad_rectangle");
        diagnostic_rectangle({q.left,q.top,q.right,q.bottom});
        for (u32 j=0;j<renderer.media_batch_count;++j) {
            const auto& batch=renderer.media_draw.batch_data[j];
            if (i>=batch.first_quad && i<batch.first_quad+batch.quad_count) {
                log_write(LogLevel::error,"probe","regional_flag_surfaces_actual_matching_quad_scissor");diagnostic_rectangle(batch.clip);
            }
        }
    }
    return false;
}
// Admitted cells have normal one-em advance and no native glyph ink. Other
// commands still emit native text, so this also rejects blanket suppression.
bool inline_cells(RegionalFlagSurfacesProbeContext& c,u64 id,u32 expected) noexcept {
    const auto& storage=*c.renderer.content_text;u32 visible=0,hidden=0;float first_top=0,last_top=0;
    if (!storage.quad_count) return diagnostic("regional_flag_surfaces_inline_no_native_quads");
    for (u32 i=0;i<c.message_text.command_count;++i) {
        const auto& command=c.message_text.commands[i];
        if (command.message_id!=id) continue;
        for (u32 s=0;s<command.styles.count;++s) {
            const auto& style=command.styles[s];
            if (!(style.flags&text_style_custom_emoji)) continue;
            if (style.flags&text_style_monospace) return diagnostic("regional_flag_surfaces_inline_monospace_suppressed",s);
            for (u32 byte=style.byte_offset;byte<style.byte_offset+style.byte_count;++byte) {
                bool admitted=false;
                for (u32 j=0;j<command.semantics.count;++j) {
                    const auto& semantic=command.semantics[j];
                    if (semantic.kind!=MessageMarkdownSemanticKind::regional_flag ||
                        byte<semantic.byte_offset || byte>=semantic.byte_offset+semantic.byte_count) continue;
                    if (semantic.byte_count!=8 || semantic.source_bytes!=8 ||
                        semantic.byte_offset+semantic.byte_count>command.text.bytes ||
                        regional_flag_sequence({command.text.data+semantic.byte_offset,semantic.byte_count})!=semantic.target)
                        return diagnostic("regional_flag_surfaces_inline_semantic_identity_range_mismatch",j);
                    admitted=true;
                }
                if (!admitted) {
                    log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_style_flags",style.flags);
                    log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_style_first",style.byte_offset);
                    log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_style_bytes",style.byte_count);
                    return diagnostic("regional_flag_surfaces_inline_unadmitted_byte_suppressed",byte);
                }
            }
        }
    }
    for (u32 i=0;i<storage.content_hit_region_count;++i) {
        const auto& r=storage.content_hit_regions[i];
        if (r.semantic_index>=storage.content_semantic_count) return diagnostic("regional_flag_surfaces_inline_semantic_index_invalid",i);
        const auto& semantic=storage.content_semantics[r.semantic_index];
        if (semantic.message_id!=id || semantic.kind!=MessageMarkdownSemanticKind::regional_flag) continue;
        if (semantic.reserved&2u) {++hidden;continue;}
        if (semantic.target!=sequences[0] && semantic.target!=sequences[1]) return diagnostic("regional_flag_surfaces_inline_unknown_target",i);
        const float extent=r.right-r.left;
        const float x=(r.left+r.right)*.5f,y=(r.top+r.bottom)*.5f;
        const UiRect cell{x-extent*.5f,y-extent*.5f,x+extent*.5f,y+extent*.5f};
        if (!within(extent,c.renderer.glyph_text.system.em_size)) {
            font_diagnostic(c.renderer);diagnostic_rectangle(cell);
            dip("regional_flag_surfaces_actual_inline_width_millidip",extent);
            dip("regional_flag_surfaces_actual_inline_region_height_millidip",r.bottom-r.top);
            log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_inline_source_offset",semantic.source_offset);
            for (u32 command_index=0;command_index<c.message_text.command_count;++command_index) {
                const auto& command=c.message_text.commands[command_index];
                for (u32 s=0;s<command.semantics.count;++s) {
                    const auto& mapped=command.semantics[s];
                    if (mapped.kind!=semantic.kind || mapped.source_offset!=semantic.source_offset) continue;
                    u32 flags=0;
                    for (u32 style=0;style<command.styles.count;++style) {
                        const auto& span=command.styles[style];
                        if (mapped.byte_offset>=span.byte_offset && mapped.byte_offset<span.byte_offset+span.byte_count) flags|=span.flags;
                    }
                    log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_inline_font_style",flags);
                    dip("regional_flag_surfaces_style_em_millidip",text_style_font_size(c.renderer.glyph_text.system.em_size,flags));
                }
            }
            return diagnostic("regional_flag_surfaces_inline_width_differs_from_em",i);
        }
        if (!quad_at(c.renderer,static_cast<u16>(semantic.target),cell,c.message_scene.viewport)) {
            font_diagnostic(c.renderer);diagnostic_rectangle(cell);
            dip("regional_flag_surfaces_actual_inline_width_millidip",extent);
            return diagnostic("regional_flag_surfaces_inline_media_rectangle_or_clip_mismatch",i);
        }
        const UiRect inner{x-extent*.2f,y-extent*.2f,x+extent*.2f,y+extent*.2f};
        for (u32 q=0;q<storage.quad_count;++q) {
            const auto& glyph=storage.quads[q];
            if (ui_rect_has_area(ui_rect_intersection(inner,{glyph.left,glyph.top,glyph.right,glyph.bottom}))) {
                diagnostic_rectangle({glyph.left,glyph.top,glyph.right,glyph.bottom});
                log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_overlapping_glyph_quad",q);
                return diagnostic("regional_flag_surfaces_inline_native_ink_inside_admitted_cell",i);
            }
        }
        if (!visible) first_top=r.top;
        last_top=maximum(last_top,r.top);++visible;
    }
    log_u64(LogLevel::info,"probe","regional_flag_surfaces_visible_inline_cells",visible);
    log_u64(LogLevel::info,"probe","regional_flag_surfaces_hidden_inline_cells",hidden);
    if (visible!=expected || hidden!=6-expected) return diagnostic("regional_flag_surfaces_inline_visible_hidden_count_mismatch");
    if (last_top-first_top<=64.f) {dip("regional_flag_surfaces_actual_inline_vertical_span_millidip",last_top-first_top);
        return diagnostic("regional_flag_surfaces_inline_wrapped_vertical_span_missing");}
    return true;
}
bool composer_cells(RegionalFlagSurfacesProbeContext& c) noexcept {
    const auto& scene=c.composer_scene;const auto& layout=c.renderer.composer_layout;
    if (!layout.active || scene.projection_span_count!=2 || scene.display_anchor!=5 || scene.display_caret!=5 ||
        layout.caret_offset!=3 || !utf8_equal(message_composer_scene_display_text(scene),draft)) {
        log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_composer_active",layout.active);
        log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_composer_spans",scene.projection_span_count);
        log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_composer_anchor",scene.display_anchor);
        log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_composer_caret",scene.display_caret);
        log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_composer_utf16_caret",layout.caret_offset);
        return diagnostic("regional_flag_surfaces_composer_projection_state_mismatch");
    }
    for (u32 byte=0;byte<=draft.bytes;++byte)
        if (message_composer_scene_display_offset_from_model(scene,byte)!=byte ||
            message_composer_scene_model_offset_from_display(scene,byte)!=byte)
            return diagnostic("regional_flag_surfaces_composer_true_byte_mapping_mismatch",byte);
    for (u32 i=0;i<2;++i) {
        const auto& span=scene.projection_spans[i];UiRect rectangle{},clip{};
        if (span.kind!=MessageComposerProjectionKind::regional_flag || span.target!=sequences[i] ||
            span.source_first!=1+9*i || span.source_last!=9+9*i || span.display_first!=span.source_first ||
            span.display_last!=span.source_last ||
            !text_composer_emoji_rectangle(layout,scene,i,&rectangle,&clip) ||
            !within(ui_rect_width(rectangle),layout.em_size) || !quad_at(c.renderer,sequences[i],rectangle,clip)) {
            diagnostic_rectangle(rectangle);dip("regional_flag_surfaces_actual_composer_width_millidip",ui_rect_width(rectangle));
            dip("regional_flag_surfaces_actual_composer_em_millidip",layout.em_size);
            log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_composer_span_kind",static_cast<u64>(span.kind));
            log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_composer_span_target",span.target);
            return diagnostic("regional_flag_surfaces_composer_span_media_geometry_mismatch",i);
        }
        u32 left=~0u,right=~0u;const float scale=c.renderer.dpi/96.f;
        const i32 y=static_cast<i32>((rectangle.top+rectangle.bottom)*.5f*scale);
        if (!renderer_message_composer_edit_hit_test(c.renderer,static_cast<i32>((rectangle.left+1.f)*scale),y,&left) ||
            !renderer_message_composer_edit_hit_test(c.renderer,static_cast<i32>((rectangle.right-1.f)*scale),y,&right) ||
            left!=span.source_first || right!=span.source_last) {
            log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_composer_left_hit",left);
            log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_composer_right_hit",right);
            diagnostic_rectangle(rectangle);
            return diagnostic("regional_flag_surfaces_composer_cell_source_endpoint_hit_mismatch",i);
        }
    }
    return true;
}
bool partial_inline_cell(RegionalFlagSurfacesProbeContext& c,u64 id) noexcept {
    const auto& storage=*c.renderer.content_text;TextHitRegion full{};u32 source=~0u;u16 sequence=0;
    for (u32 i=0;i<storage.content_hit_region_count;++i) {
        const auto& region=storage.content_hit_regions[i];
        if (region.semantic_index>=storage.content_semantic_count) return false;
        const auto& semantic=storage.content_semantics[region.semantic_index];
        if (semantic.message_id==id && semantic.kind==MessageMarkdownSemanticKind::regional_flag && !(semantic.reserved&2u)) {
            full=region;source=semantic.source_offset;sequence=static_cast<u16>(semantic.target);break;
        }
    }
    if (!sequence || source==~0u || c.message_text.command_count>kMaximumMessageTextCommands) return false;
    const float extent=full.right-full.left,x=(full.left+full.right)*.5f,y=(full.top+full.bottom)*.5f;
    const UiRect artwork{x-extent*.5f,y-extent*.5f,x+extent*.5f,y+extent*.5f};
    const UiRect viewport=c.message_scene.viewport;UiRect clips[kMaximumMessageTextCommands]{};
    const u32 command_count=c.message_text.command_count;
    const u64 signature=c.message_text.projection_signature;
    const auto* title=ui_scene_resolve(c.shell_scene,ui_scene_find(c.shell_scene,UiSemantic::title_bar));
    const auto* composer=ui_scene_resolve(c.shell_scene,ui_scene_find(c.shell_scene,UiSemantic::composer));
    if (c.shell_scene.view!=UiShellView::guild_channel || !title || !composer) return false;
    auto shell_metrics=kGuildChannelShellLayoutMetrics;
    shell_metrics.revision=c.shell_scene.layout_metrics_revision;
    shell_metrics.navigation_width=viewport.left-shell_metrics.guild_rail_width;
    shell_metrics.title_bar_height=ui_rect_height(title->rectangle);
    shell_metrics.composer_height=ui_rect_height(composer->rectangle);
    shell_metrics.message_bottom_margin=composer->rectangle.top-viewport.bottom-shell_metrics.composer_margin.top;
    const float shell_width=c.shell_scene.width,shell_height=c.shell_scene.height;
    for (u32 i=0;i<command_count;++i) clips[i]=c.message_text.commands[i].clip;
    struct RestoreClip {
        RegionalFlagSurfacesProbeContext& c;UiRect viewport;const UiRect* clips;u32 count;u64 signature;
        UiShellLayoutMetrics metrics;float width,height;
        ~RestoreClip() noexcept {
            ui_scene_layout_with_metrics(&c.shell_scene,UiShellView::guild_channel,metrics,width,height);
            ui_widget_manager_layout_rectangle(&c.message_scene.widgets,viewport);
            c.message_scene.viewport=viewport;
            for (u32 i=0;i<count;++i) c.message_text.commands[i].clip=clips[i];
            c.message_text.projection_signature=signature;
        }
    } restore{c,viewport,clips,command_count,signature,shell_metrics,shell_width,shell_height};
    for (u32 axis=0;axis<2;++axis) {
        auto cut=viewport;
        if (!axis) cut.left=x;else cut.top=y;
        auto metrics=shell_metrics;metrics.revision+=axis+1;
        if (!axis) metrics.navigation_width=x-metrics.guild_rail_width;
        else metrics.title_bar_height+=y-viewport.top;
        // The renderer requires the shell's native MessageView region and the
        // retained MessageScene viewport to agree. Resize both through their
        // layout APIs; retain the original full text rectangles as overscan.
        if (!ui_scene_layout_with_metrics(&c.shell_scene,UiShellView::guild_channel,metrics,shell_width,shell_height))
            return diagnostic("regional_flag_surfaces_partial_shell_layout_failed",axis);
        const auto* message_view=ui_scene_resolve(c.shell_scene,ui_scene_find(c.shell_scene,UiSemantic::message_view));
        if (!message_view || !ui_rect_equal(message_view->rectangle,cut)) {
            if (message_view) diagnostic_rectangle(message_view->rectangle);
            return diagnostic("regional_flag_surfaces_partial_shell_viewport_mismatch",axis);
        }
        if (!ui_widget_manager_layout_rectangle(&c.message_scene.widgets,cut).ok())
            return diagnostic("regional_flag_surfaces_partial_message_widget_layout_failed",axis);
        c.message_scene.viewport=cut;
        for (u32 i=0;i<command_count;++i)
            c.message_text.commands[i].clip=ui_rect_intersection(clips[i],cut);
        c.message_text.projection_signature=message_text_presentation_projection_signature(c.message_text);
        // Preserve each full typographic rectangle. Only the actual viewport
        // and command scissor change, as for a partly visible retained row.
        if (!renderer_rebuild_content_text(&c.renderer,kDefaultTheme) ||
            !probe_present(&c.renderer,kDefaultTheme) || !quad_at(c.renderer,sequence,artwork,cut)) return false;
        bool retained=false;
        for (u32 i=0;i<storage.content_hit_region_count;++i) {
            const auto& region=storage.content_hit_regions[i];
            if (region.semantic_index>=storage.content_semantic_count) return false;
            const auto& semantic=storage.content_semantics[region.semantic_index];
            if (semantic.message_id!=id || semantic.source_offset!=source || semantic.kind!=MessageMarkdownSemanticKind::regional_flag) continue;
            if (!within(region.left,full.left) || !within(region.right,full.right) ||
                !within(region.top,full.top) || !within(region.bottom,full.bottom)) return false;
            retained=true;
        }
        const float scale=c.renderer.dpi/96.f;
        const i32 inside_x=static_cast<i32>((x+extent*.25f)*scale);
        const i32 inside_y=static_cast<i32>((y+extent*.25f)*scale);
        const i32 outside_x=static_cast<i32>((x+(axis?extent*.25f:-extent*.25f))*scale);
        const i32 outside_y=static_cast<i32>((y+(axis?-extent*.25f:extent*.25f))*scale);
        const auto inside=renderer_content_hit_test(c.renderer,inside_x,inside_y);
        if (!retained || !inside.valid || inside.message_id!=id || inside.target!=sequence ||
            inside.source_offset!=source || renderer_message_view_hit_test(c.renderer,outside_x,outside_y) ||
            renderer_content_hit_test(c.renderer,outside_x,outside_y).valid ||
            renderer_message_text_position_hit_test(c.renderer,outside_x,outside_y).valid) return false;
    }
    log_write(LogLevel::info,"probe","regional_flag_surfaces_partial_horizontal_vertical_scissors_preserve_full_cells");
    return true;
}
} // namespace

bool regional_flag_surfaces_ui_probe(RegionalFlagSurfacesProbeContext c) noexcept {
    if (!c.add_message || !c.rebuild || !c.capture || !c.renderer.content_text ||
        c.message_view.store.count!=1 || c.popup.active || c.renderer.reaction_viewer ||
        !upload_flags(c.renderer)) return fail(__LINE__);
    const auto handle=message_store_ordered_handle(c.message_view.store,0);
    const auto* record=message_store_resolve(c.message_view.store,handle);
    if (!record || message_rich_store_find(c.rich_store,record->id)) return fail(__LINE__);
    const u64 id=record->id;const auto original=message_store_content(c.message_view.store,handle);
    char saved[1024]{},saved_draft[128]{};
    if (original.bytes>sizeof(saved) || c.composer.bytes>sizeof(saved_draft)) return fail(__LINE__);
    copy_bytes(saved,original.data,original.bytes);copy_bytes(saved_draft,c.composer.text,c.composer.bytes);
    const u32 saved_bytes=c.composer.bytes,saved_anchor=c.composer.anchor,saved_caret=c.composer.caret;
    const bool saved_focus=c.composer.focused;const u64 channel=c.composer.channel_id;
    ComposerAutocomplete autocomplete{};ReactionViewer viewer{};
    struct Restore {
        RegionalFlagSurfacesProbeContext& c;ComposerAutocomplete& autocomplete;ReactionViewer& viewer;
        u64 id,channel;Utf8View content,draft;u32 anchor,caret;bool focused;
        ~Restore() noexcept {
            // Clear renderer borrows before stack-local autocomplete/viewer die.
            c.renderer.reaction_viewer=nullptr;popup_scene_close(&c.popup);
            reaction_viewer_close(&viewer);composer_autocomplete_release(&autocomplete);
            message_rich_store_reset(&c.rich_store);c.add_message(id,content);
            message_composer_restore(&c.composer,channel,draft,anchor,caret);
            c.composer.focused=focused;c.message_text.spoiler_reveal_count=0;
        }
    } restore{c,autocomplete,viewer,id,channel,{saved,original.bytes},{saved_draft,saved_bytes},saved_anchor,saved_caret,saved_focus};
    constexpr auto denied=utf8_literal("Code `\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF` hidden ||\xF0\x9F\x87\xB9\xF0\x9F\x87\xB0|| unsupported \xF0\x9F\x87\xA6\xF0\x9F\x87\xA6");
    if (!c.add_message(id,denied) || !message_composer_restore(&c.composer,channel,{},0,0) ||
        !paint(c) || flag_quads(c.renderer)!=0 || !c.renderer.content_text->quad_count) return fail(__LINE__);
    log_write(LogLevel::info,"probe","regional_flag_surfaces_code_hidden_unsupported_media_zero");
    if (!c.add_message(id,text) || !message_composer_restore(&c.composer,channel,draft,5,5)) return fail(__LINE__);
    c.composer.focused=true;
    if (!paint(c)) {font_diagnostic(c.renderer);diagnostic("regional_flag_surfaces_initial_paint_failed");return fail(__LINE__);}
    log_write(LogLevel::info,"probe","regional_flag_surfaces_initial_paint_passed");
    if (!inline_cells(c,id,5)) {
        c.capture(L"flag-surfaces-hidden-composer.bmp");return fail(__LINE__);
    }
    log_write(LogLevel::info,"probe","regional_flag_surfaces_initial_inline_passed");
    if (!composer_cells(c)) {
        c.capture(L"flag-surfaces-hidden-composer.bmp");return fail(__LINE__);
    }
    log_write(LogLevel::info,"probe","regional_flag_surfaces_initial_composer_passed");
    const auto initial_flag_quads=flag_quads(c.renderer);
    if (initial_flag_quads!=7) {
        log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_initial_flag_quads",initial_flag_quads);
        c.capture(L"flag-surfaces-hidden-composer.bmp");
        diagnostic("regional_flag_surfaces_initial_flag_quad_count_mismatch");return fail(__LINE__);
    }
    if (!c.capture(L"flag-surfaces-hidden-composer.bmp")) return fail(__LINE__);
    if (!partial_inline_cell(c,id) || !paint(c)) return fail(__LINE__);
    MessageTextHit spoiler{};
    for (u32 i=0;i<c.message_text.command_count;++i) {
        const auto& command=c.message_text.commands[i];
        for (u32 j=0;j<command.semantics.count;++j) if (command.semantics[j].kind==MessageMarkdownSemanticKind::spoiler) {
            const auto& s=command.semantics[j];spoiler={id,s.target,command.message_revision,s.source_offset,s.source_bytes,s.kind,true};
        }
    }
    bool changed=false;
    if (!spoiler.valid || !message_text_presentation_toggle_spoiler(&c.message_text,c.message_view.store,spoiler,&changed) ||
        !changed || !paint(c) || !inline_cells(c,id,6) || flag_quads(c.renderer)!=8 ||
        !c.capture(L"flag-surfaces-revealed.bmp")) return fail(__LINE__);
    GatewayMessageReaction reactions[2]{};
    for (u32 i=0;i<2;++i) {reactions[i].name=i?tk:tj;reactions[i].count=3+i;reactions[i].normal_count=3+i;}
    GatewayMessageEvent event{};event.id=id;event.fields=gateway_message_field_reactions;event.reactions={reactions,2};
    const auto current=message_store_find(c.message_view.store,id);
    const auto* current_record=current.ok()?message_store_resolve(c.message_view.store,current.handle):nullptr;
    if (!current_record || !message_rich_store_apply(&c.rich_store,id,current_record->revision,event).ok() || !paint(c)) return fail(__LINE__);
    const auto* rich=message_rich_store_find(c.rich_store,id);
    const auto* row=message_scene_find_row(c.message_scene,0);
    if (!rich || !row) return fail(__LINE__);
    const auto* row_layout=message_scene_layout_node(c.message_scene,row->widget);
    if (!row_layout) return fail(__LINE__);
    // Reaction media inherits the native row scissor, which is narrower than
    // the message viewport when this retained row ends inside the viewport.
    const auto reaction_clip=ui_rect_intersection(c.message_scene.viewport,row_layout->rectangle);
    for (u32 i=0;i<2;++i) {
        MessageReactionVisualItem item{};
        if (!message_reaction_presentation_resolve(c.message_scene,*row,*rich,i,&item) || item.emoji_id || item.animated ||
            item.regional_flag!=sequences[i] || !quad_at(c.renderer,sequences[i],item.emoji_rectangle,reaction_clip)) return fail(__LINE__);
    }
    ReactionViewerUser viewer_users[1]{};char viewer_text[128]{};
    if (!reaction_viewer_initialize(&viewer,{viewer_users,1},{viewer_text,sizeof(viewer_text)}) ||
        !reaction_viewer_open(&viewer,c.state.selected_guild_id,channel,id,c.rich_store,*rich,
            {0,0,c.renderer.width*96.f/c.renderer.dpi,c.renderer.height*96.f/c.renderer.dpi}).ok() ||
        !renderer_bind_reaction_viewer(&c.renderer,&viewer) || !paint(c)) return fail(__LINE__);
    UiTextCommand viewer_commands[32]{};UiTextCommandStream viewer_stream{};
    UiTextOutput viewer_stream_output{&viewer_stream, nullptr};
    if (!ui_text_command_stream_initialize(&viewer_stream,{viewer_commands,32}) ||
        !reaction_viewer_text_presentation_build_output(viewer,kDefaultTheme,&viewer_stream_output)) return fail(__LINE__);
    for (u32 i=0;i<viewer_stream.count;++i)
        if (utf8_equal(viewer_commands[i].text,tj) || utf8_equal(viewer_commands[i].text,tk)) return fail(__LINE__);
    for (u32 i=0;i<2;++i) {
        const auto r=reaction_viewer_aggregate_rectangle(viewer,i);
        const float top=(r.top+r.bottom-24.f)*.5f;
        if (!utf8_equal(reaction_viewer_aggregate_name(viewer,i),i?tk:tj) ||
            !quad_at(c.renderer,sequences[i],{r.left+8,top,r.left+32,top+24},viewer.body)) return fail(__LINE__);
    }
    c.renderer.reaction_viewer=nullptr;reaction_viewer_close(&viewer);
    constexpr auto query=utf8_literal(":flag_tajikistan");
    if (!message_composer_restore(&c.composer,channel,query,query.bytes,query.bytes) || !paint(c) ||
        !composer_autocomplete_build(&autocomplete,c.composer,{},c.users,c.navigation,c.state) ||
        autocomplete.kind!=ComposerAutocompleteKind::emoji || !autocomplete.count) return fail(__LINE__);
    const auto& suggestion=autocomplete.suggestions[0];
    Utf8View canonical{};
    const auto catalog=unicode_emoji_catalog();
    for (u32 entry=0;entry<catalog.count;++entry)
        if (utf8_equal(catalog[entry].glyph(),tj)) {canonical=catalog[entry].name();break;}
    char expected_label[kUnicodeEmojiMaximumNameBytes+2]{};
    if (!canonical.data || canonical.bytes>kUnicodeEmojiMaximumNameBytes) return fail(__LINE__);
    expected_label[0]=':';copy_bytes(expected_label+1,canonical.data,canonical.bytes);
    expected_label[canonical.bytes+1]=':';
    const Utf8View expected_alias{expected_label,canonical.bytes+2};
    if (suggestion.emoji_id || suggestion.animated_emoji || suggestion.insertion_bytes!=tj.bytes+1 ||
        !utf8_equal({suggestion.insertion,tj.bytes},tj) || suggestion.insertion[tj.bytes]!=' ') return fail(__LINE__);
    if (!utf8_equal(suggestion.label,expected_alias)) {
        char actual[kComposerAutocompleteLabelBytes+1]{};
        if (suggestion.label.data && suggestion.label.bytes<=kComposerAutocompleteLabelBytes)
            copy_bytes(actual,suggestion.label.data,suggestion.label.bytes);
        log_u64(LogLevel::error,"probe","regional_flag_surfaces_actual_autocomplete_label_bytes",suggestion.label.bytes);
        log_u64(LogLevel::error,"probe","regional_flag_surfaces_expected_autocomplete_label_bytes",expected_alias.bytes);
        log_write(LogLevel::error,"probe",actual);
        diagnostic("regional_flag_surfaces_autocomplete_canonical_alias_mismatch");return fail(__LINE__);
    }
    PopupItem item{suggestion.label,701,popup_item_enabled|popup_item_emoji_preview};item.regional_flag=sequences[0];
    const auto* surface=ui_widget_manager_layout_node(c.composer_scene.widgets,c.composer_scene.surface);
    const UiRect viewport{0,0,c.renderer.width*96.f/c.renderer.dpi,c.renderer.height*96.f/c.renderer.dpi};
    if (!surface || !popup_scene_open(&c.popup,PopupKind::autocomplete,channel,{&item,1},surface->rectangle,
        viewport,ui_rect_width(surface->rectangle),true).ok() || !paint(c)) return fail(__LINE__);
    UiTextCommand autocomplete_text[4]{};UiTextCommandStream autocomplete_stream{};
    UiTextOutput autocomplete_stream_output{&autocomplete_stream, nullptr};
    if (!ui_text_command_stream_initialize(&autocomplete_stream,{autocomplete_text,4}) ||
        !popup_text_presentation_build_output(c.popup,kDefaultTheme,&autocomplete_stream_output) ||
        autocomplete_stream.count!=1 || !utf8_equal(autocomplete_text[0].text,expected_alias)) return fail(__LINE__);
    PopupMediaCommand media{};
    if (popup_media_presentation_resolve(c.popup,0,&media)!=PopupMediaPresentationStatus::ready ||
        media.regional_flag!=sequences[0] || media.media_id || media.media_flags || media.media_variant || media.avatar || media.guild_icon ||
        !quad_at(c.renderer,sequences[0],media.rectangle,media.clip) ||
        !popup_scene_select_index(&c.popup,0,&changed) || popup_scene_selected_action(c.popup)!=701 ||
        !c.capture(L"flag-surfaces-reactions-autocomplete.bmp")) return fail(__LINE__);
    log_write(LogLevel::info,"probe","regional_flag_surfaces_inline_composer_reaction_viewer_autocomplete_passed");
    return true;
}
} // namespace maicord
