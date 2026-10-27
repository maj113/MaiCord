#pragma once
#include "probe_present.hpp"
#include "ui_surface_motion_measurement.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/message_context_menu.hpp"
#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/unread_divider.hpp"

namespace maicord {
namespace unread_boundary_probe_detail {
inline constexpr u32 kRows = 4;
struct Fixture {
    MessageRecord records[kRows]{}; u32 order[kRows]{},heights[kRows]{}; u64 fenwick[kRows]{};
    u8 text[8192]{},rich_text[8192]{}; u16 rich_order[kRows]{};
    UiWidget widgets[kRows * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[kRows * kMessageSceneWidgetsPerRow + 1]{};
    MessageStore store{}; MessageRichStore rich{}; StableStorage rich_storage{};
    UiVariableList list{}; MessageTimeline timeline{}; MessageScene scene{};
    MessageTextPresentation presentation{};
    PopupScene menu{}; PopupItem items[24]{}; PopupRowWidget menu_rows[24]{};
    UiWidget menu_widgets[50]{}; UiLayoutNode menu_nodes[50]{};
    ~Fixture() noexcept { popup_scene_release(&menu); message_scene_release(&scene); message_rich_store_release(&rich); }
};
struct Geometry { UiRect line,avatar,frame,edge; };
inline Geometry geometry(const MessageScene& scene,u64 first) noexcept {
    Geometry result{};
    const auto* boundary = message_scene_find_row(scene,1);
    const auto* forwarded = message_scene_find_row(scene,2);
    if (boundary && boundary->message_id == first + 1) {
        result.line = ui_unread_divider_geometry(message_scene_unread_divider_rectangle(scene,*boundary),scene.unread_divider_style,
            scene.pixel_scale).line;
        const auto* avatar = message_scene_layout_node(scene,boundary->avatar);
        if (avatar) result.avatar = avatar->rectangle;
    }
    const auto* child = forwarded ? message_scene_snapshot_row(scene,*forwarded) : nullptr;
    const auto* frame = child ? message_scene_layout_node(scene,child->widget) : nullptr;
    if (frame) {
        result.frame = frame->rectangle;
        result.edge = {result.frame.left,result.frame.top,result.frame.left + 3,result.frame.bottom};
    }
    return result;
}
inline void log_rectangle(UiRect rectangle) noexcept {
    const float values[]{rectangle.left,rectangle.top,rectangle.right,rectangle.bottom};
    const char* names[]{"left_f32bits","top_f32bits","right_f32bits","bottom_f32bits"};
    for (u32 i = 0; i < 4; ++i) { u32 bits{}; copy_bytes(&bits,values + i,sizeof(bits)); log_u64(LogLevel::info,"unread_geometry",names[i],bits); }
}
inline bool diagnose(const D3D11Renderer& renderer,const MessageScene& scene,Geometry expected) noexcept {
    log_u64(LogLevel::info,"unread_geometry","scene_count",scene.count);
    log_u64(LogLevel::info,"unread_geometry","scene_first",scene.first);
    log_u64(LogLevel::info,"unread_geometry","paint_quads",renderer.paint_stream.count);
    log_u64(LogLevel::info,"unread_geometry","paint_batches",renderer.paint_stream.batch_count);
    log_u64(LogLevel::info,"unread_geometry","paint_clips",renderer.paint_stream.clip_count);
    for (u32 i = 0; i < scene.count; ++i) {
        const auto& row = scene.rows[i];
        log_u64(LogLevel::info,"unread_geometry","outer_message_id",row.message_id);
        log_u64(LogLevel::info,"unread_geometry","outer_grouped",row.grouped);
        const auto* node = message_scene_layout_node(scene,row.widget);
        if (node) { log_write(LogLevel::info,"unread_geometry","outer_row_rectangle"); log_rectangle(node->rectangle); }
        node = message_scene_layout_node(scene,row.avatar);
        if (node) { log_write(LogLevel::info,"unread_geometry","outer_avatar_rectangle"); log_rectangle(node->rectangle); }
    }
    const UiRect rectangles[]{expected.line,expected.avatar,expected.frame,expected.edge};
    const char* names[]{"divider_line","boundary_avatar","snapshot_frame","snapshot_edge"};
    bool found = true;
    for (u32 target = 0; target < 4; ++target) {
        log_write(LogLevel::info,"unread_geometry",names[target]); log_rectangle(rectangles[target]);
        u32 matches = 0;
        for (u32 i = 0; i < renderer.paint_stream.count; ++i) {
            const auto& q = renderer.paint_stream.quads[i];
            if (!ui_rect_equal({q.left,q.top,q.right,q.bottom},rectangles[target])) continue;
            ++matches; log_u64(LogLevel::info,"unread_geometry","matching_quad",i);
            u32 alpha{}; copy_bytes(&alpha,&q.fill.alpha,sizeof(alpha));
            log_u64(LogLevel::info,"unread_geometry","fill_alpha_f32bits",alpha);
            for (u32 b = 0; b < renderer.paint_stream.batch_count; ++b) {
                const auto& batch = renderer.paint_stream.batches[b];
                if (i < batch.first_quad || i - batch.first_quad >= batch.quad_count) continue;
                log_u64(LogLevel::info,"unread_geometry","matching_batch",b);
                const auto clip = renderer.paint_stream.clips[batch.clip_index];
                log_write(LogLevel::info,"unread_geometry","matching_batch_clip");
                log_rectangle({clip.left,clip.top,clip.right,clip.bottom});
            }
        }
        log_u64(LogLevel::info,"unread_geometry","matching_quads",matches);
        found &= ui_rect_has_area(rectangles[target]) && matches != 0;
    }
    return found;
}
inline bool pixels(D3D11Renderer& renderer,const MessageScene& scene,const Theme& theme,Geometry g) noexcept {
    // Re-draw the retained stream into the current flip-model target, then read
    // that target. Counts alone cannot prove that emitted geometry was drawn.
    if (!d3d11_renderer_draw_stream(&renderer,theme,true,true,nullptr)) return false;
    surface_motion_probe_detail::Readback rb{};
    if (!rb.initialize(renderer) || !rb.read(renderer,0)) return false;
    const float scale = renderer.dpi / 96.f;
    const float ax = (g.avatar.left + g.avatar.right) * .5f,ay = (g.avatar.top + g.avatar.bottom) * .5f;
    const UiRect samples[]{g.line,{ax - 4,ay - 4,ax + 4,ay + 4},
        {g.frame.right - 1,g.frame.top + 12,g.frame.right,g.frame.bottom - 12},
        {g.edge.left + 1,g.edge.top + 12,g.edge.right - 1,g.edge.bottom - 12}};
    const Color colors[]{theme.notification,theme.avatar_placeholder,theme.interactive,theme.interactive_hover};
    const char* names[]{"divider_rule_pixels","boundary_avatar_pixels","snapshot_frame_pixels","snapshot_edge_pixels"};
    bool valid = true;
    for (u32 target = 0; target < 4; ++target) {
        const auto region = raster_clip_rectangle(ui_rect_intersection(samples[target],scene.viewport),scale,rb.width,rb.height);
        const auto color = colors[target];
        const u8 expected[]{static_cast<u8>(color.blue * 255.f + .5f),static_cast<u8>(color.green * 255.f + .5f),static_cast<u8>(color.red * 255.f + .5f)};
        u32 ink = 0;
        for (i32 y = maximum(region.top,0); y < region.bottom; ++y) for (i32 x = maximum(region.left,0); x < region.right; ++x) {
            const auto* p = rb.images + (y * rb.width + x) * 4;
            const i32 bx = target == 0 ? x : target == 1 ? static_cast<i32>((g.avatar.left - 4) * scale) :
                target == 2 ? static_cast<i32>((g.frame.right + 3) * scale) : static_cast<i32>((g.edge.left - 4) * scale);
            const i32 by = target == 0 ? static_cast<i32>((g.line.top - 3) * scale) : y;
            if (bx < 0 || by < 0 || bx >= static_cast<i32>(rb.width) || by >= static_cast<i32>(rb.height)) continue;
            const auto* background = rb.images + (by * rb.width + bx) * 4;
            // Partial one-pixel coverage is valid, but it must move the actual
            // pixel away from adjacent background toward this semantic color.
            if (surface_motion_probe_detail::distance(p,background) >= 8 &&
                surface_motion_probe_detail::distance(p,expected) + 8 < surface_motion_probe_detail::distance(background,expected)) ++ink;
        }
        log_u64(ink >= 8 ? LogLevel::info : LogLevel::error,"unread_pixels",names[target],ink);
        valid &= ink >= 8;
    }
    // The rule is snapped to whole device pixels: every covered row carries the
    // full colour and the rows just outside it stay untouched background.
    const auto line = ui_rect_intersection(g.line,scene.viewport);
    const i32 top = static_cast<i32>(line.top * scale + .5f),bottom = static_cast<i32>(line.bottom * scale + .5f);
    const Color rule = theme.notification;
    const u8 expected[]{static_cast<u8>(rule.blue * 255.f + .5f),static_cast<u8>(rule.green * 255.f + .5f),static_cast<u8>(rule.red * 255.f + .5f)};
    u32 crisp = 0,columns = 0;
    for (i32 x = static_cast<i32>(line.left * scale) + 4; x < static_cast<i32>(line.right * scale) - 4; x += 37) {
        if (top < 3 || bottom + 1 >= static_cast<i32>(rb.height) || x >= static_cast<i32>(rb.width)) break;
        const auto* background = rb.images + ((top - 3) * rb.width + x) * 4;
        bool column = bottom - top >= 1;
        for (i32 y = top; y < bottom; ++y)
            column &= surface_motion_probe_detail::distance(rb.images + (y * rb.width + x) * 4,expected) <= 6;
        column &= surface_motion_probe_detail::distance(rb.images + ((top - 1) * rb.width + x) * 4,background) <= 6 &&
                  surface_motion_probe_detail::distance(rb.images + (bottom * rb.width + x) * 4,background) <= 6;
        crisp += column;
        ++columns;
    }
    log_u64(crisp == columns && columns ? LogLevel::info : LogLevel::error,"unread_pixels","divider_rule_crisp_columns",crisp);
    return valid && columns && crisp == columns;
}
}
// This fixture exercises native presentation and PopupScene input/policy. The
// authenticated App dispatch/admission contract belongs to its runtime fixture.
inline bool unread_boundary_ui_probe(D3D11Renderer& renderer,UiTextMeasureCache& measure,
    bool (*capture)(const wchar_t*,const Theme&) noexcept) noexcept {
    using namespace unread_boundary_probe_detail;
    if (!renderer.message_scene || !renderer.message_text || !renderer.chat_state ||
        !renderer.navigation_view || !renderer.ui_scene || !renderer.content_text || !renderer.dpi) return false;
    Fixture f{};
    const u32 width = renderer.width,height = renderer.height,dpi = renderer.dpi;
    struct Restore {
        D3D11Renderer& renderer; const MessageScene* scene; const MessageTextPresentation* text;
        const PopupScene* popup; const UiWidgetManager* focus;
        ~Restore() noexcept {
            renderer.message_scene = scene; renderer.message_text = text; renderer.focus_owner = focus;
            renderer_bind_popup_scene(&renderer,popup); renderer.retained_presentation.valid = false;
            content_text_retention_invalidate(&renderer.content_text_retention);
        }
    } restore{renderer,renderer.message_scene,renderer.message_text,renderer.popup_scene,renderer.focus_owner};
    const auto require = [&](bool ok,u32 stage) noexcept {
        if (!ok) log_u64(LogLevel::error,"probe","unread_boundary_native_failure_stage",stage);
        return ok;
    };
    constexpr u64 first = 0x554e5201;
    const u64 channel = renderer.navigation_view->selected_channel_id;
    if (!require(channel && message_store_initialize(&f.store,{f.records,kRows},{f.order,kRows},{f.text,sizeof(f.text)}),1)) return false;
    const Utf8View bodies[]{utf8_literal("The interface review is ready. These notes have already been read."),
        utf8_literal("A new message from the same author starts here. The unread boundary restores the full author header."),
        utf8_literal("Forwarded design notes for the next review."),
        utf8_literal("The reply remains below its own header.\n\nKeyboard navigation can reach this final message in a short window.\nThe divider belongs to the outer timeline message.\nIts label is native measured text, independent of the message body.\nScrolling keeps the decoration inside the timeline clip.\nThere are no added divider widgets or cache owners.\nOriginal notes remain selectable below the divider.\nEnd of the review.")};
    for (u32 i = 0; i < kRows; ++i)
        if (!require(message_store_upsert(&f.store,{first + i,channel,123,1'725'021'300'000ull + i * 30'000ull,0,bodies[i],0}).ok(),2)) return false;
    if (!require(message_rich_store_initialize_owned_sparse(&f.rich,&f.rich_storage,kRows,{f.rich_order,kRows},{f.rich_text,sizeof(f.rich_text)}) &&
        ui_variable_list_initialize(&f.list,{f.heights,kRows},{f.fenwick,kRows},kRows,ui_extent_units(80)) &&
        message_timeline_initialize(&f.timeline,&f.store,&f.list,ui_extent_units(80),ui_extent_units(700),false) &&
        message_timeline_bind_rich_store(&f.timeline,&f.rich) &&
        message_scene_initialize_owned(&f.scene,&f.timeline,kRows,1,{f.widgets,kRows * kMessageSceneWidgetsPerRow + 1},
            {f.nodes,kRows * kMessageSceneWidgetsPerRow + 1}) &&
        popup_scene_initialize(&f.menu,{f.items,24},{f.menu_rows,24},{f.menu_widgets,50},{f.menu_nodes,50}),3)) return false;
    GatewayMessageEvent forwarded{}; forwarded.id = first + 2;
    forwarded.fields = gateway_message_field_message_snapshots | gateway_message_field_message_reference;
    forwarded.reference.type = 1; forwarded.reference.channel_id = 8; forwarded.reference.message_id = 90;
    forwarded.snapshot.present = true; forwarded.snapshot.fields = gateway_message_field_content;
    forwarded.snapshot.content = utf8_literal("**Frozen original:** This snapshot has no independent unread divider.\nThe retained source text belongs inside this outer message.");
    GatewayMessageEvent reply{}; reply.id = first + 3; reply.fields = gateway_message_field_message_reference;
    reply.reference = {first + 1,channel,0,123,utf8_literal("Mai"),{},bodies[1],GatewayMessageReferenceState::available,0};
    if (!require(message_rich_store_apply(&f.rich,first + 2,f.records[2].revision,forwarded).ok() &&
        message_rich_store_apply(&f.rich,first + 3,f.records[3].revision,reply).ok(),4)) return false;
    f.presentation = *restore.text; f.presentation.active = false; f.presentation.projection_signature = 0;
    f.presentation.command_count = f.presentation.measurement_count = 0;
    renderer.message_scene = &f.scene; renderer.message_text = &f.presentation;
    renderer_bind_popup_scene(&renderer,nullptr); renderer.focus_owner = nullptr;
    Theme theme = kDefaultTheme;
    const auto project = [&]() noexcept {
        f.scene.pixel_scale = renderer.dpi / 96.f;
        if (!message_scene_update(&f.scene,ui_scene_content_rectangle(*renderer.ui_scene,UiSemantic::message_view)).ok()) return false;
        // Surface resizing republishes the bootstrap shell's original scene.
        // Rebind this borrowed fixture before both geometry and text converge.
        if (!renderer_bind_message_scene(&renderer,&f.scene)) return false;
        renderer.message_text = &f.presentation;
        if (renderer.message_scene != &f.scene || renderer.message_text != &f.presentation) return false;
        return message_text_coordinator_converge({&f.scene,&f.presentation,&measure,renderer.chat_state,
            renderer.user_store,renderer.navigation_view,&theme,
            +[](void* owner,const Theme& current) noexcept { return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner),current); },
            &renderer,1'725'021'300ull,static_cast<u16>(renderer.dpi),nullptr}).ok();
    };
    const auto show = [&](const wchar_t* name) noexcept {
        return project() && probe_present(&renderer,theme) && (!capture || capture(name,theme));
    };
    renderer_set_dpi(&renderer,96);
    if (!require(probe_resize_surface(1200,800) && project(),5)) return false;
    const auto* grouped = message_scene_find_row(f.scene,1);
    if (!require(grouped && grouped->grouped,6)) return false;
    const u32 widget_count = f.scene.widgets.count,row_capacity = f.scene.owned_rows.capacity;
    bool changed{};
    if (!require(message_scene_set_unread_cursor(&f.scene,channel,first,true,&changed) && changed &&
        show(L"unread-default.bmp") && f.scene.widgets.count == widget_count &&
        f.scene.owned_rows.capacity == row_capacity,7)) return false;
    const auto default_geometry = geometry(f.scene,first);
    if (!require(diagnose(renderer,f.scene,default_geometry),24) ||
        !require(pixels(renderer,f.scene,theme,default_geometry),25)) return false;
    const auto verify_boundary = [&]() noexcept {
        u32 dividers = 0,labels = 0;
        for (u32 i = 0; i < f.scene.count; ++i) {
            const auto& row = f.scene.rows[i]; const auto strip = message_scene_unread_divider_rectangle(f.scene,row);
            if (ui_rect_has_area(strip)) {
                ++dividers;
                const auto body = message_scene_message_rectangle(f.scene,row);
                if (row.message_id != first + 1 || row.grouped || body.top != strip.bottom ||
                    ui_rect_height(strip) != kDefaultUiUnreadDividerStyle.height) return false;
                const auto child = message_scene_snapshot_row(f.scene,row);
                if (child && ui_rect_has_area(message_scene_unread_divider_rectangle(f.scene,*child))) return false;
                const auto visible = ui_rect_intersection(strip,f.scene.viewport);
                if (ui_rect_has_area(visible)) {
                    const float y = (visible.top + visible.bottom) * .5f;
                    const float xs[]{visible.left + 2,(visible.left + visible.right) * .5f,visible.right - 2};
                    for (const auto x : xs) {
                        const auto px = static_cast<i32>(x * renderer.dpi / 96.f),py = static_cast<i32>(y * renderer.dpi / 96.f);
                        if (message_scene_hit_test(f.scene,x,y).message_id || message_scene_reply_hit_test(f.scene,x,y).message_id ||
                            renderer_message_text_position_hit_test(renderer,px,py).valid || renderer_content_hit_test(renderer,px,py).valid) return false;
                    }
                }
            }
            const auto child = message_scene_snapshot_row(f.scene,row);
            if (child && ui_rect_has_area(message_scene_unread_divider_rectangle(f.scene,*child))) return false;
        }
        for (u32 i = 0; i < f.presentation.command_count; ++i) {
            const auto& command = f.presentation.commands[i];
            if (!utf8_equal(command.text,kUiUnreadDividerLabel)) continue;
            ++labels; const auto color = ui_unread_divider_text_color(theme);
            if (command.message_id || command.measurement_kind != MessageTextMeasurementKind::none ||
                command.measurement_index != kInvalidMessageTextMeasurement ||
                command.measured_width <= 0 || command.measured_height <= 0 ||
                // Rows clip to themselves and may extend past the viewport,
                // which bounds their paint in the renderer.
                command.clip.left < f.scene.viewport.left || command.clip.right > f.scene.viewport.right ||
                command.color.red != color.red || command.color.green != color.green || command.color.blue != color.blue) return false;
        }
        return dividers == 1 && labels == 1;
    };
    if (!require(verify_boundary(),8)) return false;
    const u32 prepare = f.scene.preparation_passes,layout = f.scene.layout_passes,resolve = f.scene.rebuilds,
        unread_revision = f.scene.unread_revision,shape = measure.backend_calls,builds = renderer.content_text->rebuilds,
        raster = renderer.glyph_text.cache_builds,paint_uploads = renderer.paint_instance_uploads,
        text_uploads = renderer.text_instance_uploads,media_uploads = renderer.media_instance_uploads,
        draws = renderer.painted_frames,skips = renderer.unchanged_present_skips;
    for (u32 i = 0; i < 64; ++i)
        if (!message_scene_set_unread_cursor(&f.scene,channel,first,true) || !project() || !probe_present(&renderer,theme)) return require(false,9);
    if (!require(f.scene.preparation_passes == prepare && f.scene.layout_passes == layout && f.scene.rebuilds == resolve &&
        f.scene.unread_revision == unread_revision && measure.backend_calls == shape && renderer.content_text->rebuilds == builds &&
        renderer.glyph_text.cache_builds == raster && renderer.paint_instance_uploads == paint_uploads &&
        renderer.text_instance_uploads == text_uploads && renderer.media_instance_uploads == media_uploads &&
        renderer.painted_frames == draws && renderer.unchanged_present_skips - skips == 64,10)) return false;
    log_write(LogLevel::info,"probe","unread_64_zero_resolve_layout_shape_raster_upload_draw_passed");
    const auto show_menu = [&](ConstSlice<PopupItem> items,u64 owner,u32 action,const wchar_t* name) noexcept {
        const UiRect full{0,0,renderer.width * 96.f / renderer.dpi,renderer.height * 96.f / renderer.dpi};
        const UiRect anchor{f.scene.viewport.left + 32,f.scene.viewport.top + 40,f.scene.viewport.left + 33,f.scene.viewport.top + 41};
        if (!popup_scene_open(&f.menu,PopupKind::context_menu,owner,items,anchor,full,280).ok() ||
            !renderer_bind_popup_scene(&renderer,&f.menu)) return false;
        bool reached = false;
        for (u32 i = 0; i <= items.count; ++i) {
            if (popup_scene_selected_action(f.menu) == action) { reached = true; break; }
            bool selection_changed{};
            if (!popup_scene_move_selection(&f.menu,1,&selection_changed)) return false;
        }
        if (!reached) return false;
        UiMenuRowGeometry geometry{};
        if (!popup_scene_menu_row_geometry(f.menu,f.menu.selected_index,&geometry) || !ui_rect_has_area(geometry.primary) ||
            !show(name)) return false;
        popup_scene_close(&f.menu); renderer_bind_popup_scene(&renderer,nullptr); return true;
    };
    MessageContextMenuModel menu{};
    if (!require(probe_resize_surface(1200,480) &&
        message_context_menu_build(&menu,f.records[1],false,false,false,nullptr,false,false,true,true).ok(),11)) return false;
    bool message_label = false;
    for (u32 i = 0; i < menu.count; ++i)
        if (menu.items[i].action == static_cast<u32>(MessageContextAction::mark_unread))
            message_label = utf8_equal(menu.items[i].label,utf8_literal("Mark unread")) && (menu.items[i].flags & popup_item_enabled);
    if (!require(message_label &&
        show_menu({menu.items,menu.count},first + 1,static_cast<u32>(MessageContextAction::mark_unread),L"unread-message-menu.bmp"),11)) return false;
    PopupItem channel_items[4]{};
    const auto channel_count = channel_context_policy_build(*renderer.chat_state,121,701,702,{channel_items,4},0,703);
    bool channel_label = false;
    for (u32 i = 0; i < channel_count; ++i)
        if (channel_items[i].action == 703)
            channel_label = utf8_equal(channel_items[i].label,utf8_literal("Mark as unread")) && (channel_items[i].flags & popup_item_enabled);
    if (!require(channel_label && show_menu({channel_items,channel_count},121,703,L"unread-channel-menu.bmp") &&
        probe_resize_surface(1200,800) && project(),12)) return false;
    const float outside = renderer.width * 96.f / renderer.dpi - ui_rect_width(f.scene.viewport);
    if (!require(probe_resize_surface(static_cast<u32>(outside + 360),520) && show(L"unread-narrow.bmp") && verify_boundary() &&
        pixels(renderer,f.scene,theme,geometry(f.scene,first)),13)) return false;
    const auto* boundary_row = message_scene_find_row(f.scene,1);
    const auto strip = boundary_row ? message_scene_unread_divider_rectangle(f.scene,*boundary_row) : UiRect{};
    const u32 shell_replays = renderer.shell_segment.replays, shell_builds = renderer.shell_segment.builds;
    if (!require(ui_rect_has_area(strip) && message_scene_scroll_by(&f.scene,
        static_cast<i64>(ui_extent_units(maximum(1.f,strip.top - f.scene.viewport.top + 12.f))),0,&changed) && changed &&
        show(L"unread-scrolled-clip.bmp") && verify_boundary(),14)) return false;
    // Only the timeline scrolled: that build replayed everything painted
    // before the timeline instead of building it.
    if (!require(renderer.shell_segment.replays == shell_replays + 1 && renderer.shell_segment.builds == shell_builds,
        24)) return false;
    // A scroll animation step moves the rastered message glyphs by whole
    // pixels instead of rastering them; the step that settles rasters in place.
    const auto scroll_step = [&](float dips, bool moving) noexcept {
        MessageTextCoordinatorRequest request{&f.scene,&f.presentation,&measure,renderer.chat_state,
            renderer.user_store,renderer.navigation_view,&theme,
            +[](void* owner,const Theme& current) noexcept { return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner),current); },
            &renderer,1'725'021'300ull,static_cast<u16>(renderer.dpi),nullptr};
        request.scroll_refresh = true;
        request.scroll_moving = moving;
        return message_scene_scroll_by(&f.scene,-static_cast<i64>(ui_extent_units(dips)),0,&changed) && changed &&
               message_text_coordinator_converge(request).ok() && probe_present(&renderer,theme);
    };
    // The first step rasters the rows near the viewport; later steps move them.
    const u32 built = f.presentation.builds;
    if (!require(scroll_step(3.f,true) && f.presentation.builds == built && renderer.text_message_shiftable,28))
        return false;
    const auto& text = *renderer.content_text;
    const u32 message_quad = renderer.content_text_prefix.shell.quads,quads = text.quad_count,
        rebuilds = text.rebuilds,glyph_rasters = renderer.glyph_text.cache_builds,projections = f.presentation.builds;
    const float quad_top = message_quad < quads ? text.quads[message_quad].top : 0.f;
    if (!require(renderer.dpi == 96 && message_quad < quads && scroll_step(5.f,true) &&
        f.presentation.builds == projections && text.rebuilds == rebuilds + 1 &&
        renderer.glyph_text.cache_builds == glyph_rasters && text.quad_count == quads &&
        renderer.text_message_shift_px == 5 && text.quads[message_quad].top == quad_top + 5.f && verify_boundary(),26))
        return false;
    // At rest only the visible rows stay rastered, at their exact position.
    if (!require(scroll_step(2.5f,false) && f.presentation.builds == projections &&
        renderer.text_message_shift_px == 0 && !renderer.text_message_shiftable && text.quad_count <= quads &&
        verify_boundary(),27)) return false;
    // A projection rebuilt with unchanged text keeps the glyphs rastered from
    // the previous build; the next scroll steps must still move them.
    if (!require(project() && project() && renderer.text_message_builds != f.presentation.builds,29))
        return false;
    if (!require(scroll_step(5.f,true) && renderer.text_message_builds == f.presentation.builds && verify_boundary(),30))
        return false;
    if (!require(scroll_step(2.5f,false) && renderer.text_message_builds == f.presentation.builds &&
        renderer.text_message_shift_px == 0 && renderer.text_message_shift_base == f.presentation.scroll_shift_dips,31))
        return false;
    const auto last = message_scene_navigate(&f.scene,UiListMove::last);
    if (!require(last.message_id == first + 3 && project(),15)) return false;
    const auto* last_row = message_scene_find_row(f.scene,3);
    if (!require(last_row && ui_rect_has_area(ui_rect_intersection(message_scene_message_rectangle(f.scene,*last_row),f.scene.viewport)) &&
        message_scene_navigate(&f.scene,UiListMove::first).message_id == first && project(),16)) return false;
    renderer_set_dpi(&renderer,144);
    if (!require(probe_resize_surface(1440,1050) && show(L"unread-dpi144.bmp") && verify_boundary() &&
        pixels(renderer,f.scene,theme,geometry(f.scene,first)),17)) return false;
    theme = kLightTheme;
    if (!require(show(L"unread-light.bmp") && verify_boundary() && pixels(renderer,f.scene,theme,geometry(f.scene,first)),18)) return false;
    theme.accent = {.95f,.85f,.4f,1}; theme.notification = theme.accent;
    if (!require(show(L"unread-light-bright-accent.bmp") && verify_boundary() && pixels(renderer,f.scene,theme,geometry(f.scene,first)),19)) return false;
    if (!require(message_scene_set_unread_cursor(&f.scene,channel,first + 3,true,&changed) && changed &&
        show(L"unread-cleared.bmp") && !f.scene.unread_first_message_id,20)) return false;
    for (u32 i = 0; i < f.presentation.command_count; ++i)
        if (utf8_equal(f.presentation.commands[i].text,kUiUnreadDividerLabel)) return require(false,21);
    if (!require(message_scene_set_unread_cursor(&f.scene,channel,0,false) && project() && !f.scene.unread_first_message_id,22)) return false;
    renderer_set_dpi(&renderer,dpi);
    if (!require(probe_resize_surface(width,height),23)) return false;
    log_write(LogLevel::info,"probe","unread_outer_boundary_native_measurement_menu_keyboard_scroll_themes_dpi_clear_passed");
    return true;
}
} // namespace maicord
