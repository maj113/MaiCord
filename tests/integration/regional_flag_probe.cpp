#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/media/regional_flag_svg.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "colr_binding_probe.hpp"
#include "regional_flag_upload_probe.hpp"
#include "probe_present.hpp"
#include "standalone_title_geometry.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"

namespace maicord {
namespace {
// One ASCII menu row, then the four flag cells.
constexpr u32 label_count = 1, item_count = label_count + 4, widget_count = 2 + item_count * kPopupWidgetsPerItem;
constexpr float popup_width = 432;
constexpr Utf8View flags[]{
    utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF"), // TJ
    utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xB0"), // TK
    utf8_literal("\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xB7\xF3\xA0\x81\xAC\xF3\xA0\x81\xB3\xF3\xA0\x81\xBF"), // Wales
    utf8_literal("\xF0\x9F\x87\xA8\xF0\x9F\x87\xB6") // CQ, Sark
};
constexpr u16 sequences[]{504, 505, 679, 69};

bool fail(u32 line) noexcept {
    log_u64(LogLevel::error,"probe","regional_flag_failure_line",line);
    return false;
}

// The directory supplies the independently verified offline corpus. Only the
// fixed metadata filename is appended; no corpus enumeration or URL is used.
bool read_flag(const wchar_t* directory, u32 chars, u16 sequence, Slice<u8> bytes,
               u32* count) noexcept {
    char target[kRegionalFlagSvgTargetBytes]{};
    const u32 target_bytes=regional_flag_svg_target(sequence,{target,sizeof(target)});
    if (!count || !target_bytes || chars > 977)
        return false;
    wchar_t path[1024]{};
    copy_bytes(path,directory,chars*sizeof(wchar_t));
    path[chars++]=L'\\';
    for (u32 i=kRegionalFlagSvgTargetPrefixBytes;i<target_bytes;++i) path[chars++]=static_cast<wchar_t>(target[i]);
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};DWORD read=0;
    const bool valid=GetFileSizeEx(file,&size) && size.QuadPart>0 &&
        size.QuadPart<=kRegionalFlagSvgMaximumBytes && size.QuadPart<=bytes.count &&
        ReadFile(file,bytes.data,static_cast<DWORD>(size.QuadPart),&read,nullptr) &&
        read==static_cast<DWORD>(size.QuadPart);
    const bool closed=CloseHandle(file)!=FALSE;
    if (!valid || !closed) return false;
    *count=read;return true;
}

bool readback_flags(D3D11Renderer& renderer, const MediaAtlasResult (&allocations)[4],
                    u16 width,u16 height,u64 (&hashes)[4]) noexcept {
    auto* context=renderer.graphics.context;
    ID3D11Texture2D* staging=nullptr;
    struct Release {ID3D11Texture2D*& texture;~Release() noexcept {com_release(texture);}} release{staging};
    D3D11_TEXTURE2D_DESC description{};renderer.media_gpu.texture->GetDesc(&description);
    if (description.Format!=DXGI_FORMAT_R8G8B8A8_UNORM || description.Width!=width || description.Height!=height)
        return false;
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=description.MiscFlags=0;
    description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if (FAILED(renderer.graphics.device->CreateTexture2D(&description,nullptr,&staging))) return false;
    context->CopyResource(staging,renderer.media_gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped))) return false;
    bool valid=true;
    for (u32 i=0;i<4;++i) {
        const auto& a=allocations[i];u32 opaque=0,transparent=0,colored=0,red=0,green=0,blue=0,yellow=0,white=0;
        hashes[i]=kHash64Seed;
        for (u32 y=0;y<64;++y) for (u32 x=0;x<64;++x) {
            const auto* p=static_cast<const u8*>(mapped.pData)+(a.y+y)*mapped.RowPitch+(a.x+x)*4;
            hashes[i]=hash64_bytes(hashes[i],p,4);
            opaque+=p[3]==255;transparent+=p[3]==0;
            valid=valid && p[0]<=p[3] && p[1]<=p[3] && p[2]<=p[3];
            if (p[3]<240) continue;
            colored+=maximum(p[0],maximum(p[1],p[2]))-minimum(p[0],minimum(p[1],p[2]))>32;
            red+=p[0]>150 && p[1]<110 && p[2]<110;
            green+=p[1]>80 && p[1]>p[0]+20 && p[1]>p[2]+20;
            blue+=p[2]>p[0]+20 && p[2]>p[1]+10;
            yellow+=p[0]>100 && p[1]>80 && p[2]<70;
            white+=p[0]>230 && p[1]>230 && p[2]>230;
        }
        valid=valid && opaque>300 && transparent>64 && colored>64;
        valid=valid && (i==0 ? red>24 && green>24 : i==1 ? blue>24 && yellow>24 :
                       i==2 ? red>24 && green>24 : red>24 && white>24);
        for (u32 previous=0;previous<i;++previous) valid=valid && hashes[previous]!=hashes[i];
        log_u64(LogLevel::info,"probe","regional_flag_rgba_hash",hashes[i]);
    }
    context->Unmap(staging,0);return valid;
}

bool draw_grid(D3D11Renderer& renderer,PopupScene& scene,UiScene& title,u32 dpi,
               const PopupItem (&items)[item_count],const MediaAtlasResult (&allocations)[4]) noexcept {
    renderer_set_dpi(&renderer,dpi);
    log_u64(LogLevel::info,"probe","regional_flag_grid_dpi",dpi);
    const UiRect viewport{0,0,renderer.width*96.f/dpi,renderer.height*96.f/dpi};
    if (ui_rect_width(viewport)<popup_width+16 || ui_rect_height(viewport)<360) return fail(__LINE__);
    if (!standalone_title_layout(&title,viewport.right,viewport.bottom)) return fail(__LINE__);
    if (!popup_scene_open(&scene,PopupKind::popover,1,{items,item_count},{16,48,17,49},viewport,
                          popup_width).ok()) return fail(__LINE__);
    UiTextCommand commands[item_count]{};UiTextCommandStream text{};
    if (!ui_text_command_stream_initialize(&text,{commands,item_count}) ||
        !popup_text_presentation_build(scene,kDefaultTheme,&text)) return fail(__LINE__);
    // Menu labels are ASCII; none of the four admitted flag
    // sequences may enter the text stream (including the subdivision tags).
    for (u32 i=0;i<text.count;++i) for (u32 flag=0;flag<4;++flag)
        if (utf8_equal(commands[i].text,flags[flag])) return fail(__LINE__);
    u32 visible_labels=0;
    for (u32 i=0;i<label_count;++i) {PopupItemLayout layout{};
        if (!popup_scene_resolve_item_layout(scene,i,&layout)) return fail(__LINE__);
        visible_labels+=layout.visible;}
    if (!visible_labels || text.count!=visible_labels) {
        log_u64(LogLevel::error,"probe","regional_flag_grid_visible_labels",visible_labels);
        log_u64(LogLevel::error,"probe","regional_flag_grid_text_commands",text.count);return fail(__LINE__);}
    for (u32 i=0;i<text.count;++i) for (u32 n=0;n<commands[i].text.bytes;++n)
        if (static_cast<u8>(commands[i].text.data[n])>=128) return fail(__LINE__);
    if (!paint_stream_reset(&renderer.paint_stream,viewport.right,viewport.bottom)) return fail(__LINE__);
    if (!popup_presentation_build(scene,kDefaultTheme,&renderer.paint_stream)) return fail(__LINE__);
    if (!renderer_media_draw_storage_reserve_quads(&renderer.media_draw,4) ||
        !renderer_media_draw_storage_reserve_batches(&renderer.media_draw,4)) return fail(__LINE__);
    media_atlas_begin_visibility(renderer.media_atlas);
    renderer.media_quad_count=renderer.media_batch_count=0;
    for (u32 i=0;i<4;++i) {
        PopupMediaCommand command{};
        if (popup_media_presentation_resolve(scene,label_count+i,&command)!=PopupMediaPresentationStatus::ready ||
            command.regional_flag!=sequences[i] || command.avatar || command.guild_icon ||
            command.media_id || command.media_variant || command.media_flags ||
            !ui_rect_has_area(ui_rect_intersection(command.rectangle,command.clip))) return fail(__LINE__);
        const auto key=media_asset_regional_flag(command.regional_flag);
        const auto found=media_atlas_find(renderer.media_atlas,key);
        const auto& a=allocations[i];
        if (!found.ok() || found.handle.slot!=a.handle.slot || found.handle.generation!=a.handle.generation ||
            key.kind!=MediaAssetKind::regional_flag || key.id!=sequences[i] || key.variant || key.flags) return fail(__LINE__);
        const auto& r=command.rectangle;
        renderer.media_draw.quad_data[i]={r.left,r.top,r.right,r.bottom,
            (a.x+.5f)/renderer.media_gpu.width,(a.y+.5f)/renderer.media_gpu.height,
            (a.x+63.5f)/renderer.media_gpu.width,(a.y+63.5f)/renderer.media_gpu.height,1,0,0,0,{}};
        renderer.media_draw.handle_data[i]=a.handle;renderer.media_draw.domain_data[i]=0;
        renderer.media_draw.batch_data[i]={command.clip,i,1};
        ++renderer.media_quad_count;++renderer.media_batch_count;
    }
    renderer.overlay_paint_first_batch=0;renderer.popup_media_first_batch=0;
    renderer.menu_paint_first_batch=renderer.paint_stream.batch_count;
    renderer.menu_media_first_batch=renderer.media_batch_count;
    const float clear[]{.03f,.03f,.03f,1};
    renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target,clear);
    if (!d3d11_renderer_draw_stream(&renderer,kDefaultTheme,true,true,nullptr,10000)) return fail(__LINE__);
    if (renderer.media_draws_last_frame!=4) {
        log_u64(LogLevel::error,"probe","regional_flag_grid_media_draws",renderer.media_draws_last_frame);return fail(__LINE__);}
    return true;
}
} // namespace

// Optional early, offline-only probe. It proves the flag artwork/command/native
// draw path through popup emoji cells, without creating an account or running
// a media worker. Root UIProbe owns the window, renderer and capture.
bool regional_flag_ui_probe(D3D11Renderer& renderer,PopupScene& scene,
                           bool(*capture)(const wchar_t*) noexcept) noexcept {
    wchar_t directory[1000]{};
    const DWORD chars=GetEnvironmentVariableW(L"MAICORD_REGIONAL_FLAG_REFERENCE_DIRECTORY",directory,1000);
    if (!chars) return true;
    if (chars>=1000 || !capture || !renderer.graphics.device || !renderer.graphics.context1 ||
        renderer.media_atlas || renderer.preview_media_atlas || renderer.media_gpu.texture ||
        renderer.content_text || scene.owned_storage.base || scene.items.data || scene.widgets.storage.data ||
        renderer.media_quad_count || renderer.media_batch_count || !colr_compute_bindings_empty(renderer.graphics.context))
        return fail(__LINE__);
    Arena scratch{};UiScene title{};MediaAtlas atlas{};
    MediaAtlasEntry entries[8]{};GlyphAtlasEntry allocations_storage[8]{};GlyphAtlasRow rows[8]{};u32 buckets[16]{};
    const auto* saved_popup=renderer.popup_scene;const auto* saved_scene=renderer.ui_scene;
    const u32 saved_dpi=renderer.dpi;
    const u32 saved_markers[]{renderer.overlay_paint_first_batch,renderer.popup_media_first_batch,
        renderer.menu_paint_first_batch,renderer.menu_media_first_batch};
    struct Release {
        D3D11Renderer& renderer;PopupScene& popup;UiScene& title;Arena& scratch;
        const PopupScene* previous_popup;const UiScene* previous_scene;u32 dpi;bool bound=false,initialized=false;
        const u32* markers;
        ~Release() noexcept {
            // Erase every renderer borrow before releasing local CPU/GPU owners.
            renderer.popup_scene=previous_popup;renderer.ui_scene=previous_scene;
            if (bound) renderer_unbind_media_atlas(&renderer);
            if (initialized) popup_scene_release(&popup);
            ui_scene_release(&title);arena_release(&scratch);renderer_set_dpi(&renderer,dpi);
            paint_stream_reset(&renderer.paint_stream,renderer.width*96.f/dpi,renderer.height*96.f/dpi);
            renderer.overlay_paint_first_batch=markers[0];renderer.popup_media_first_batch=markers[1];
            renderer.menu_paint_first_batch=markers[2];renderer.menu_media_first_batch=markers[3];
        }
    } release{renderer,scene,title,scratch,saved_popup,saved_scene,saved_dpi,false,false,saved_markers};
    constexpr usize scratch_bytes=kRegionalFlagSvgMaximumBytes+widget_count*(sizeof(UiWidget)+sizeof(UiLayoutNode))+256;
    if (!arena_initialize(&scratch,scratch_bytes,0,4096)) return fail(__LINE__);
    auto* encoded=arena_allocate<u8>(&scratch,kRegionalFlagSvgMaximumBytes);
    auto* widgets=arena_allocate_zero<UiWidget>(&scratch,widget_count);
    auto* nodes=arena_allocate_zero<UiLayoutNode>(&scratch,widget_count);
    if (!encoded || !widgets || !nodes || !popup_scene_initialize_owned(&scene,item_count,{widgets,widget_count},{nodes,widget_count}))
        return fail(__LINE__);
    release.initialized=true;
    if (!ui_scene_initialize_bootstrap(&title) || !standalone_title_layout(&title,renderer.width*96.f/saved_dpi,renderer.height*96.f/saved_dpi) ||
        !renderer_bind_ui_scene(&renderer,&title) || !renderer_bind_popup_scene(&renderer,&scene) ||
        !media_atlas_initialize(&atlas,256,256,1,{entries,8},{buckets,16},{allocations_storage,8},{rows,8}) ||
        !renderer_bind_media_atlas(&renderer,&atlas)) return fail(__LINE__);
    release.bound=true;
    D3D11GlyphGpuFootprint before{},after{};
    if (!d3d11_glyph_text_gpu_footprint(renderer.glyph_text,&before) ||
        d3d11_glyph_text_color_view(renderer.glyph_text)) return fail(__LINE__);
    MediaAtlasResult allocations[4]{};
    for (u32 i=0;i<4;++i) {
        u32 bytes=0;
        if (regional_flag_sequence(flags[i])!=sequences[i] || !read_flag(directory,chars,sequences[i],{encoded,kRegionalFlagSvgMaximumBytes},&bytes))
            return fail(__LINE__);
        const auto result=probe_upload_regional_flag(&renderer,media_asset_regional_flag(sequences[i]),{encoded,bytes});
        if (!result.ok() || result.allocation.width!=64 || result.allocation.height!=64 ||
            !colr_compute_bindings_empty(renderer.graphics.context)) return fail(__LINE__);
        allocations[i]=result.allocation;
    }
    u64 square_hashes[4]{};
    if (!d3d11_glyph_text_gpu_footprint(renderer.glyph_text,&after) || after.texture_bytes!=before.texture_bytes ||
        d3d11_glyph_text_color_view(renderer.glyph_text) || after.color_dispatches-before.color_dispatches!=4 ||
        !readback_flags(renderer,allocations,256,256,square_hashes)) return fail(__LINE__);
    log_u64(LogLevel::info,"probe","regional_flag_shared_texture_bytes",after.texture_bytes);
    log_u64(LogLevel::info,"probe","regional_flag_program_buffer_bytes",after.buffer_bytes-before.buffer_bytes);
    log_u64(LogLevel::info,"probe","regional_flag_media_texture_bytes",256*256*4);
    PopupItem items[item_count]{};items[0]={utf8_literal("Search flags"),1,popup_item_enabled};
    for (u32 i=0;i<4;++i) items[label_count+i]={flags[i],10+i,popup_item_enabled|popup_item_emoji};
    constexpr u32 dpis[]{96,144,192};constexpr const wchar_t* filenames[]{L"flag-grid-96.bmp",L"flag-grid-144.bmp",L"flag-grid-192.bmp"};
    for (u32 i=0;i<3;++i) {
        if (!draw_grid(renderer,scene,title,dpis[i],items,allocations)) return fail(__LINE__);
        if (!capture(filenames[i])) return fail(__LINE__);
    }
    popup_scene_close(&scene);
    // A tall atlas reproduces the former square-width rejection: with padding,
    // later flag rows extend below y=128. The same shared program/shader owner
    // must bake every tile into the actual 128x512 caller texture identically.
    renderer_unbind_media_atlas(&renderer);release.bound=false;
    if (atlas.entry_count || renderer.media_gpu.texture ||
        !media_atlas_initialize(&atlas,128,512,1,{entries,8},{buckets,16},{allocations_storage,8},{rows,8}) ||
        !renderer_bind_media_atlas(&renderer,&atlas)) return fail(__LINE__);
    release.bound=true;
    D3D11GlyphGpuFootprint tall_before{},tall_after{};
    if (!d3d11_glyph_text_gpu_footprint(renderer.glyph_text,&tall_before)) return fail(__LINE__);
    for (u32 i=0;i<4;++i) {
        u32 bytes=0;
        if (!read_flag(directory,chars,sequences[i],{encoded,kRegionalFlagSvgMaximumBytes},&bytes)) return fail(__LINE__);
        const auto result=probe_upload_regional_flag(&renderer,media_asset_regional_flag(sequences[i]),{encoded,bytes});
        if (!result.ok() || result.allocation.width!=64 || result.allocation.height!=64 ||
            !colr_compute_bindings_empty(renderer.graphics.context)) return fail(__LINE__);
        allocations[i]=result.allocation;
    }
    u64 tall_hashes[4]{};
    if (allocations[2].y+allocations[2].height<=128 || allocations[3].y+allocations[3].height<=128 ||
        !d3d11_glyph_text_gpu_footprint(renderer.glyph_text,&tall_after) ||
        tall_after.texture_bytes!=tall_before.texture_bytes || tall_after.buffer_bytes!=tall_before.buffer_bytes ||
        tall_after.color_dispatches-tall_before.color_dispatches!=4 || d3d11_glyph_text_color_view(renderer.glyph_text) ||
        !readback_flags(renderer,allocations,128,512,tall_hashes)) return fail(__LINE__);
    for (u32 i=0;i<4;++i) if (tall_hashes[i]!=square_hashes[i]) return fail(__LINE__);
    log_write(LogLevel::info,"probe","regional_flag_tall_128x512_four_hashes_shared_program_and_empty_compute_bindings_passed");
    renderer.popup_scene=saved_popup;renderer.ui_scene=saved_scene;
    renderer_unbind_media_atlas(&renderer);release.bound=false;
    popup_scene_release(&scene);release.initialized=false;
    if (renderer.media_atlas || renderer.media_gpu.texture || renderer.media_gpu.texture_view ||
        renderer.media_gpu.texture_uav || atlas.entry_count || scene.owned_storage.base || scene.items.data ||
        !colr_compute_bindings_empty(renderer.graphics.context)) return fail(__LINE__);
    log_write(LogLevel::info,"probe","regional_flag_four_assets_rgba_popup_commands_three_dpi_native_draw_and_release_passed");
    return true;
}

} // namespace maicord
