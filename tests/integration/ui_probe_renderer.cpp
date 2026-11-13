#include "ui_probe_internal.hpp"
#include "maicord/core/log_event.hpp"
#include "maicord/ui/shell/caption_bar.hpp"
#include "probe_capture.hpp"
#include "probe_surface.hpp"
#include "standalone_title_geometry.hpp"
#include "maicord/ui/chat/composer/attachment_menu.hpp"
#include "maicord/ui/text/edit_menu.hpp"
#include "unread_boundary_ui_probe.hpp"
#include "thread_browser_ui_probe.hpp"

namespace maicord {
namespace ui_probe_detail {

// Popup overlays through the hardware path: keyboard reveal, attachment and
// text edit menus, plugin and settings popups, then the compact hover bar.
bool probe_popup_overlays() noexcept {
    g_probe.failure_stage = 71;
    const bool initialized = popup_scene_initialize_owned(
        &g_probe.popup_scene, kProbePopupItemCapacity,
        Slice<UiWidget>{g_probe.popup_widgets, 2 + kProbePopupItemCapacity * kPopupWidgetsPerItem},
        Slice<UiLayoutNode>{g_probe.popup_layout_nodes, 2 + kProbePopupItemCapacity * kPopupWidgetsPerItem});
    if (!initialized || !renderer_bind_popup_scene(&g_probe.renderer, &g_probe.popup_scene))
        return false;
    const UiRect window{0, 0, g_probe.renderer.width * 96.0f / g_probe.renderer.dpi,
                        g_probe.renderer.height * 96.0f / g_probe.renderer.dpi};
    const UiRect messages = g_probe.message_scene.viewport;
    const UiRect viewport = ui_rect_intersection(
        window, {ui_rect_width(messages) >= 448.0f ? messages.left : g_probe.shell_navigation_scene.viewport.left,
                 messages.top, messages.right, messages.bottom});

    // Keyboard reveal must update the retained popup's clipped text through
    // the real hardware presentation path, including reverse wrap.
    g_probe.failure_stage = 87;
    PopupItem suggestions[8]{};
    for (u32 i = 0; i < 8; ++i) {
        suggestions[i] = {i == 0   ? utf8_literal("First result")
                          : i == 7 ? utf8_literal("Last result")
                                   : utf8_literal("Navigation result"),
                          1000u + i, popup_item_enabled, 0};
    }
    const UiRect compact{viewport.left, viewport.top,
                         viewport.left + minimum(288.0f, ui_rect_width(viewport)),
                         viewport.top + minimum(128.0f, ui_rect_height(viewport))};
    if (!popup_scene_open(&g_probe.popup_scene, PopupKind::autocomplete, 78, {suggestions, 8},
                          {compact.left + 8, compact.bottom - 8, compact.right - 8, compact.bottom - 7},
                          compact, ui_rect_width(compact) - 16, true)
             .ok())
        return false;
    for (u32 pass = 0; pass < 2; ++pass) {
        bool changed = false;
        const bool selected = pass == 0 ? popup_scene_move_selection(&g_probe.popup_scene, -1, &changed)
                                        : popup_scene_select_index(&g_probe.popup_scene, 0, &changed);
        const auto* row = popup_scene_layout_node(g_probe.popup_scene,
                                                  g_probe.popup_scene.rows[pass == 0 ? 7 : 0].widget);
        const auto* panel = popup_scene_layout_node(g_probe.popup_scene, g_probe.popup_scene.panel);
        if (!selected || !changed || !row || !panel || row->rectangle.top < panel->content_rectangle.top ||
            row->rectangle.bottom > panel->content_rectangle.bottom || !rebuild_probe_content_text() ||
            !probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        bool row_ink = false;
        for (u32 b = g_probe.renderer.overlay_text_first_batch; b < g_probe.text.batch_count; ++b) {
            const auto& batch = g_probe.text.batches[b];
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const auto& quad = g_probe.text.quads[q];
                if (quad.top < panel->content_rectangle.top ||
                    quad.bottom > panel->content_rectangle.bottom)
                    return false;
                row_ink = row_ink || ui_rect_contains(row->rectangle, (quad.left + quad.right) * 0.5f,
                                                      (quad.top + quad.bottom) * 0.5f);
            }
        }
        if (!row_ink)
            return false;
    }
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text())
        return false;

    // Use the same per-file view as the shipping attachment controls.
    PopupItem file_items[kAttachmentFileMenuItems];
    for (u32 spoiler = 0; spoiler < 2; ++spoiler) {
        g_probe.failure_stage = 88;
        if (attachment_file_menu(utf8_literal("旅行の写真.png"), spoiler != 0,
                                 {file_items, kAttachmentFileMenuItems}) != kAttachmentFileMenuItems ||
            !popup_scene_open(
                 &g_probe.popup_scene, PopupKind::popover, 79, {file_items, kAttachmentFileMenuItems},
                 {viewport.right - 24, viewport.bottom - 24, viewport.right - 23, viewport.bottom - 23},
                 viewport, 360, true)
                 .ok() ||
            !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme))
            return false;
        PopupItemLayout control{};
        if (!popup_scene_resolve_item_layout(g_probe.popup_scene, 1, &control) || !control.visible ||
            popup_scene_selected_action(g_probe.popup_scene) != popup_action_attachment_spoiler ||
            ((g_probe.popup_scene.items[1].flags & popup_item_checked) != 0) != (spoiler != 0))
            return false;
        const auto hit = popup_scene_hit_test(
            g_probe.popup_scene, (control.visible_rectangle.left + control.visible_rectangle.right) * 0.5f,
            (control.visible_rectangle.top + control.visible_rectangle.bottom) * 0.5f);
        if (!hit.enabled || hit.action != popup_action_attachment_spoiler)
            return false;
    }
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text())
        return false;
    log_write(LogLevel::info, "probe", "attachment_spoiler_menu_hardware_passed");
    PopupItem edit_items[kTextEditMenuItems]{};
    text_edit_menu_build({false, false, true, true, true, true, true, false}, {edit_items, kTextEditMenuItems});
    if (!popup_scene_open(
             &g_probe.popup_scene, PopupKind::context_menu, 80, {edit_items, kTextEditMenuItems},
             {viewport.right - 24, viewport.bottom - 24, viewport.right - 23, viewport.bottom - 23},
             viewport, 260)
             .ok() ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme) ||
        popup_scene_selected_action(g_probe.popup_scene) != static_cast<u32>(TextEditCommand::cut))
        return false;
    UiTextCommand edit_commands[14]{};
    UiTextCommandStream edit_stream{};
    UiTextOutput edit_stream_output{&edit_stream, nullptr};
    ui_text_command_stream_initialize(&edit_stream, {edit_commands, 14});
    if (!popup_text_presentation_build_output(g_probe.popup_scene, kDefaultTheme, &edit_stream_output) ||
        edit_stream.count != 14)
        return false;
    for (u32 i = 0; i < edit_stream.count; i += 2) {
        if (edit_commands[i].horizontal_alignment != UiTextAlignment::end ||
            edit_commands[i].rectangle.left < edit_commands[i + 1].rectangle.right)
            return false;
    }
    const auto menu_builds = g_probe.renderer.paint_stream_builds;
    if (!probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.paint_stream_builds != menu_builds)
        return false;
    popup_scene_close(&g_probe.popup_scene);
    if (!rebuild_probe_content_text())
        return false;
    log_write(LogLevel::info, "probe", "text_edit_menu_shortcuts_hardware_and_retention_passed");
    if (!plugin_ui_probe(g_probe.renderer, g_probe.popup_scene, probe_capture_frame))
        return false;
    if (!plugin_navigation_ui_probe(g_probe.renderer, g_probe.popup_scene, probe_capture_frame))
        return false;
    if (!settings_choice_ui_probe(g_probe.renderer, g_probe.popup_scene, probe_capture_frame))
        return false;

    // Exercise the compact hover bar through the same hardware text path.
    // Its 28-DIP targets intentionally retain the same 24-DIP emoji em used
    // by the full picker and the 44-DIP context-menu targets.
    g_probe.failure_stage = 84;
    const MessageSceneRowWidget* bar_row =
        g_probe.message_scene.count != 0 ? &g_probe.message_scene.rows[0] : nullptr;
    const MessageRecord* bar_message =
        bar_row ? message_store_resolve(g_probe.message_view.store, bar_row->message) : nullptr;
    const MessageActionBarResult bar_opened =
        bar_message ? message_action_bar_open(
                          &g_probe.message_action_bar, g_probe.message_scene,
                          MessageSceneHit{bar_row->message_id, bar_row->widget, bar_row->source_index},
                          *bar_message)
                    : MessageActionBarResult{};
    if (!bar_opened.ok() ||
        !renderer_bind_message_action_bar(&g_probe.renderer, &g_probe.message_action_bar) ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme)) {
        return false;
    }
    g_probe.failure_stage = 85;
    bool hover_glyph = false;
    for (u32 batch_index = g_probe.renderer.overlay_text_first_batch;
         batch_index < g_probe.text.batch_count; ++batch_index) {
        const GlyphDrawBatch& batch = g_probe.text.batches[batch_index];
        if (batch.first_quad > g_probe.text.quad_count ||
            batch.quad_count > g_probe.text.quad_count - batch.first_quad) {
            return false;
        }
        for (u32 index = 0; index < batch.quad_count; ++index) {
            const GlyphQuad& quad = g_probe.text.quads[batch.first_quad + index];
            if (quad.left < g_probe.message_action_bar.rectangle.left ||
                quad.top < g_probe.message_action_bar.rectangle.top ||
                quad.right > g_probe.message_action_bar.rectangle.right ||
                quad.bottom > g_probe.message_action_bar.rectangle.bottom) {
                return false;
            }
            hover_glyph = true;
        }
    }
    for (u32 cell = 0; cell < 3; ++cell) {
        const UiRect owner = g_probe.message_action_bar.items[cell].rectangle;
        UiRect ink{owner.right, owner.bottom, owner.left, owner.top};
        bool found = false;
        for (u32 b = g_probe.renderer.overlay_text_first_batch; b < g_probe.text.batch_count; ++b) {
            const GlyphDrawBatch& batch = g_probe.text.batches[b];
            for (u32 q = batch.first_quad; q < batch.first_quad + batch.quad_count; ++q) {
                const GlyphQuad& quad = g_probe.text.quads[q];
                if (!ui_rect_contains(owner, (quad.left + quad.right) * 0.5f,
                                      (quad.top + quad.bottom) * 0.5f))
                    continue;
                found = true;
                ink.left = minimum(ink.left, quad.left);
                ink.top = minimum(ink.top, quad.top);
                ink.right = maximum(ink.right, quad.right);
                ink.bottom = maximum(ink.bottom, quad.bottom);
            }
        }
        const float dx = (ink.left + ink.right) - (owner.left + owner.right);
        const float dy = (ink.top + ink.bottom) - (owner.top + owner.bottom);
        if (!found || dx < -0.01f || dx > 0.01f || dy < -0.01f || dy > 0.01f)
            return false;
    }
    g_probe.failure_stage = 86;
    bool bar_changed = false;
    const u32 stale_overlay_suppressions = g_probe.renderer.stale_interaction_text_suppressions;
    if (!hover_glyph || ui_rect_height(g_probe.message_action_bar.rectangle) != 32.0f ||
        ui_rect_width(g_probe.message_action_bar.items[0].rectangle) != kMessageActionBarCellExtent ||
        !message_action_bar_close(&g_probe.message_action_bar, &bar_changed) || !bar_changed ||
        !probe_present(&g_probe.renderer, kDefaultTheme) ||
        g_probe.renderer.stale_interaction_text_suppressions != stale_overlay_suppressions + 1u ||
        !rebuild_probe_content_text() || !probe_present(&g_probe.renderer, kDefaultTheme)) {
        return false;
    }
    g_probe.failure_stage = 0;
    return true;
}

} // namespace ui_probe_detail

namespace {

LRESULT CALLBACK probe_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (post_resize_probe_detail::window_message(window, message, wparam, lparam))
        return 0;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_ENTERSIZEMOVE:
        renderer_set_live_resize(&g_probe.renderer, true);
        return 0;
    case WM_SIZE:
        if (g_probe.renderer_ready && wparam != SIZE_MINIMIZED) {
            ++g_probe.resize_messages;
            const bool resized = renderer_resize(&g_probe.renderer, LOWORD(lparam), HIWORD(lparam));
            const bool shell_ready = resized && synchronize_probe_shell_layout();
            const bool text_ready =
                shell_ready && (!g_probe.renderer.content_text || rebuild_probe_content_text());
            if (text_ready && g_probe.renderer.content_text) {
                g_probe.resize_min_text_quads =
                    minimum(g_probe.resize_min_text_quads, g_probe.renderer.content_text->quad_count);
            }
            if (!text_ready ||
                (g_probe.renderer.content_text && g_probe.renderer.content_text->quad_count == 0) ||
                (g_probe.login_view_bound && !set_probe_login_view(g_probe.login_view)) ||
                !probe_present(&g_probe.renderer, kDefaultTheme)) {
                DestroyWindow(window);
            }
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (g_probe.renderer_ready) {
            RECT client{};
            if (!GetClientRect(window, &client) ||
                !renderer_finish_resize(&g_probe.renderer,
                                        static_cast<u32>(maximum<LONG>(1, client.right - client.left)),
                                        static_cast<u32>(maximum<LONG>(1, client.bottom - client.top)))) {
                DestroyWindow(window);
            }
        }
        return 0;
    case WM_PAINT: {
        ++g_probe.paint_messages;
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        const bool ok = !g_probe.renderer_ready || probe_present(&g_probe.renderer, kDefaultTheme);
        EndPaint(window, &paint);
        if (!ok) {
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

namespace ui_probe_detail {

bool create_probe_window() noexcept {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t kClassName[] = L"MaiCordUiProbeWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = 0;
    window_class.lpfnWndProc = probe_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kClassName;
    if (!RegisterClassExW(&window_class)) {
        return false;
    }
    RECT bounds{0, 0, 1200, 970};
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, kProbeWindowExStyle);
    g_probe.window = CreateWindowExW(kProbeWindowExStyle, kClassName, L"MaiCord M38 UI Probe",
                                     WS_OVERLAPPEDWINDOW, 32, 32, bounds.right - bounds.left,
                                     bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    return g_probe.window != nullptr;
}

bool once_requested() noexcept {
    return probe_option_enabled(L"MAICORD_UI_PROBE_ONCE");
}

bool device_owner_empty(const D3D11DeviceOwner& owner) noexcept {
    const GpuSurface& surface = owner.surface;
    return !owner.device && !owner.context && !owner.context1 && !surface.swap_chain &&
           !surface.composition_device && !surface.composition_target && !surface.composition_visual &&
           !surface.target && !surface.frame_latency_waitable && surface.buffer_width == 0 &&
           surface.buffer_height == 0 && surface.resize_buffer_reallocations == 0;
}

} // namespace ui_probe_detail

namespace {

// A waitable swap chain queues one frame by default, which the owner relies on instead of setting it.
bool default_frame_latency_is_one(const D3D11DeviceOwner& owner) noexcept {
    IDXGISwapChain2* swap_chain = nullptr;
    UINT latency = 0;
    const bool queried =
        SUCCEEDED(owner.surface.swap_chain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&swap_chain))) &&
        SUCCEEDED(swap_chain->GetMaximumFrameLatency(&latency));
    com_release(swap_chain);
    return queried && latency == 1;
}

} // namespace

namespace ui_probe_detail {

bool device_owner_ready(const D3D11Renderer& renderer) noexcept {
    const D3D11DeviceOwner& owner = renderer.graphics;
    const GpuSurface& surface = owner.surface;
    return owner.device && owner.context && owner.context1 && surface.swap_chain &&
           surface.composition_device && surface.composition_target && surface.composition_visual &&
           surface.target && surface.frame_latency_waitable && surface.buffer_width == renderer.width &&
           surface.buffer_height == renderer.height && default_frame_latency_is_one(owner);
}

bool draw_pipeline_empty(const D3D11DrawPipelineOwner& owner) noexcept {
    return !owner.ui_vertex_shader && !owner.ui_pixel_shader && !owner.ui_input_layout &&
           !owner.ui_instance_buffer && !owner.ui_frame_buffer && !owner.ui_rasterizer_state &&
           !owner.ui_blend_state && !owner.glyph_vertex_shader && !owner.glyph_pixel_shader &&
           !owner.media_pixel_shader && !owner.glyph_input_layout && !owner.glyph_instance_buffer &&
           !owner.media_instance_buffer && !owner.glyph_sampler && owner.glyph_instance_capacity == 0;
}

bool draw_pipeline_ready(const D3D11Renderer& renderer) noexcept {
    const D3D11DrawPipelineOwner& owner = renderer.pipeline;
    return owner.ui_vertex_shader && owner.ui_pixel_shader && owner.ui_input_layout &&
           owner.ui_instance_buffer && owner.ui_frame_buffer && owner.ui_rasterizer_state &&
           owner.ui_blend_state && owner.glyph_vertex_shader && owner.glyph_pixel_shader &&
           owner.media_pixel_shader && owner.glyph_input_layout && owner.glyph_instance_buffer &&
           owner.media_instance_buffer && owner.glyph_sampler &&
           owner.glyph_instance_capacity >= kRendererGlyphCapacity;
}

bool glyph_text_owner_empty(const D3D11GlyphTextOwner& owner) noexcept {
    bool faces_empty = owner.font_face_count == 0;
    for (u32 index = 0; owner.font_faces && index < owner.font_face_capacity; ++index) {
        faces_empty = faces_empty && !owner.font_faces[index].native_face.slot && owner.font_faces[index].id == 0 &&
                      owner.font_faces[index].flags == 0;
        faces_empty = faces_empty && owner.font_faces[index].empty_glyph_plus_one == 0;
    }
    return !owner.color_batch && !owner.storage.base && !owner.system.native_text && !owner.system.native_fonts &&
           owner.system.em_size == 0.0f && !owner.color_glyphs && !owner.title.glyphs &&
           !owner.title.runs && !owner.title.clusters && !owner.allocator.entries &&
           !owner.allocator.rows && !owner.gpu.texture && !owner.gpu.surface && !owner.gpu.texture_view &&
           !owner.gpu.texture_uav && !owner.gpu.command_buffer && !owner.gpu.command_view &&
           !owner.gpu.raster_shader && owner.gpu.dispatched_pixels == 0 &&
           owner.gpu.dispatched_commands == 0 && owner.gpu.dispatch_count == 0 &&
           owner.title_quad_count == 0 && owner.cache_builds == 0 && !owner.ready && faces_empty;
}

bool glyph_text_owner_ready(const D3D11Renderer& renderer) noexcept {
    const D3D11GlyphTextOwner& owner = renderer.glyph_text;
    const bool system_ready = owner.system.native_fonts && owner.system.native_text && owner.system.native_format;
    return owner.storage.base && owner.glyphs && owner.runs && owner.clusters &&
           owner.path_commands && owner.atlas_entries && owner.atlas_rows && owner.title_quads &&
           owner.font_faces && system_ready && owner.title.glyphs && owner.title.runs &&
           owner.title.clusters && owner.allocator.entries && owner.allocator.rows && owner.gpu.surface &&
           owner.gpu.texture_view && owner.gpu.command_buffer &&
           owner.gpu.command_view && owner.gpu.raster_shader && owner.ready;
}

bool message_hit_cache_empty(const MessageHitTestCacheOwner& owner) noexcept {
    return !owner.geometry && !owner.native_format && !owner.storage.base && owner.message_id == 0 &&
           owner.measurement_identity == 0 && owner.message_revision == 0 && owner.command_index == 0 &&
           owner.utf16_units == 0 && owner.builds == 0 && owner.hits == 0 && owner.rejected == 0 &&
           owner.width == 0.0f && owner.height == 0.0f && owner.em_size == 0.0f;
}

bool composer_layout_empty(const TextEditLayoutOwner& owner) noexcept {
    return !owner.geometry && !owner.format && owner.text_signature == 0 && owner.text_bytes == 0 &&
           owner.anchor == 0 && owner.caret_offset == 0 && owner.utf16_units == 0 &&
           owner.selection_count == 0 && owner.builds == 0 && owner.hits == 0 && owner.rejected == 0 &&
           owner.origin_x == 0.0f && owner.origin_y == 0.0f && owner.width == 0.0f &&
           owner.height == 0.0f && owner.em_size == 0.0f && !owner.active;
}

bool content_text_retention_empty(const ContentTextRetentionOwner& owner) noexcept {
    const ContentTextRetentionKey& key = owner.key;
    return !owner.valid && owner.builds == 0 && owner.hits == 0 && owner.misses == 0 &&
           owner.invalidations == 0 && key.shell_signature == 0 && key.message_signature == 0 &&
           key.popup_signature == 0 && key.theme_signature == 0 && key.selection_message_id == 0 &&
           key.selection_command_identity == 0 && key.selection_revision == 0 &&
           key.selection_command_index == 0 && key.selection_anchor == 0 && key.selection_caret == 0 &&
           key.dpi == 0 && key.atlas_evicted_entries == 0 && key.atlas_full_resets == 0 &&
           !key.include_messages;
}

bool resize_stress_requested() noexcept {
    return probe_option_enabled(L"MAICORD_UI_PROBE_RESIZE_STRESS");
}

bool run_resize_stress() noexcept {
    g_probe.failure_stage = 110;
    constexpr u32 kHalfSteps = 32;
    constexpr u32 kSteps = kHalfSteps * 2;
    constexpr LONG kInitialWidth = 1200;
    constexpr LONG kInitialHeight = 970;
    constexpr LONG kFinalWidth = kInitialWidth;
    constexpr LONG kFinalHeight = 360;
    const u32 clipped_updates_before = g_probe.renderer.resize_clipped_updates;
    const u32 reallocations_before = g_probe.renderer.graphics.surface.resize_buffer_reallocations;
    const u32 resize_messages_before = g_probe.resize_messages;
    const u32 paint_messages_before = g_probe.paint_messages;
    const u32 painted_frames_before = g_probe.renderer.painted_frames;
    const u32 text_rejections_before =
        g_probe.renderer.content_text ? g_probe.renderer.content_text->rejected : 0;
    u64 geometry_signatures[kHalfSteps + 1]{};
    geometry_signatures[0] = probe_resize_geometry_signature();
    g_probe.resize_min_text_quads = ~0u;

    renderer_set_live_resize(&g_probe.renderer, true);
    for (u32 step = 1; step <= kSteps; ++step) {
        const u32 distance_step = step <= kHalfSteps ? step : kSteps - step;
        const LONG client_width =
            kInitialWidth -
            static_cast<LONG>((static_cast<u64>(kInitialWidth - kFinalWidth) * distance_step) / kHalfSteps);
        const LONG client_height =
            kInitialHeight -
            static_cast<LONG>((static_cast<u64>(kInitialHeight - kFinalHeight) * distance_step) /
                              kHalfSteps);
        RECT bounds{0, 0, client_width, client_height};
        if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, kProbeWindowExStyle) ||
            !SetWindowPos(g_probe.window, nullptr, 0, 0, bounds.right - bounds.left,
                          bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER) ||
            !UpdateWindow(g_probe.window) || !IsWindow(g_probe.window)) {
            log_u64(LogLevel::error, "probe", "resize_stress_failed_step", step);
            return false;
        }
        const u64 signature = probe_resize_geometry_signature();
        if (step <= kHalfSteps) {
            geometry_signatures[distance_step] = signature;
        } else if (geometry_signatures[distance_step] != signature) {
            log_u64(LogLevel::error, "probe", "resize_geometry_mismatch_step", step);
            return false;
        }
    }

    RECT client{};
    g_probe.failure_stage = 111;
    if (!GetClientRect(g_probe.window, &client) || client.right <= client.left ||
        client.bottom <= client.top ||
        !renderer_finish_resize(&g_probe.renderer, static_cast<u32>(client.right - client.left),
                                static_cast<u32>(client.bottom - client.top)) ||
        !probe_present(&g_probe.renderer, kDefaultTheme)) {
        return false;
    }

    g_probe.failure_stage = 112;
    const u32 resize_messages = g_probe.resize_messages - resize_messages_before;
    const u32 paint_messages = g_probe.paint_messages - paint_messages_before;
    if (resize_messages != kSteps) {
        g_probe.failure_stage = 113;
        return false;
    }
    if (paint_messages > resize_messages) {
        g_probe.failure_stage = 114;
        return false;
    }
    // A client that still fits the buffers is clipped, never reallocated:
    // shrinking and returning to the initial size keeps the original chain.
    if (g_probe.renderer.resize_clipped_updates - clipped_updates_before != kSteps ||
        g_probe.renderer.graphics.surface.resize_buffer_reallocations != reallocations_before) {
        log_u64(LogLevel::error, "probe", "resize_clipped_updates",
                g_probe.renderer.resize_clipped_updates - clipped_updates_before);
        log_u64(LogLevel::error, "probe", "resize_buffer_reallocations",
                g_probe.renderer.graphics.surface.resize_buffer_reallocations - reallocations_before);
        g_probe.failure_stage =
            g_probe.renderer.resize_clipped_updates - clipped_updates_before != kSteps ? 115 : 116;
        return false;
    }
    // Each WM_SIZE must paint once. The final present is deliberately allowed
    // to be a retained no-op after returning to the initial geometry.
    if (g_probe.renderer.painted_frames - painted_frames_before != kSteps) {
        log_u64(LogLevel::error, "probe", "resize_painted_frames",
                g_probe.renderer.painted_frames - painted_frames_before);
        log_u64(LogLevel::error, "probe", "resize_expected_painted_frames", kSteps);
        log_u64(LogLevel::error, "probe", "resize_paint_messages", paint_messages);
        g_probe.failure_stage = 117;
        return false;
    }
    if (g_probe.resize_min_text_quads == ~0u || g_probe.resize_min_text_quads == 0) {
        g_probe.failure_stage = 118;
        return false;
    }
    if (g_probe.renderer.content_text &&
        g_probe.renderer.content_text->rejected != text_rejections_before) {
        g_probe.failure_stage = 119;
        return false;
    }
    if (g_probe.renderer.live_resize) {
        g_probe.failure_stage = 120;
        return false;
    }
    g_probe.failure_stage = 0;
    return true;
}

bool initialize_probe_renderer() noexcept {
    LARGE_INTEGER begin{}, end{}, frequency{};
    ULONG64 first_cycles = 0, last_cycles = 0;
    QueryPerformanceFrequency(&frequency);
    QueryThreadCycleTime(GetCurrentThread(), &first_cycles);
    QueryPerformanceCounter(&begin);
    const bool result = renderer_initialize(&g_probe.renderer, probe_window_surface(g_probe.window), true);
    QueryPerformanceCounter(&end);
    QueryThreadCycleTime(GetCurrentThread(), &last_cycles);
    log_u64(LogLevel::info, "bootstrap", "renderer_init_us",
            static_cast<u64>(end.QuadPart - begin.QuadPart) * 1'000'000 / frequency.QuadPart);
    log_u64(LogLevel::info, "bootstrap", "renderer_init_cycles", last_cycles - first_cycles);
    return result;
}

bool probe_bootstrap_title() noexcept {
    auto& renderer = g_probe.renderer;
    if (!ui_scene_initialize_bootstrap(&g_probe.shell_scene))
        return false;
    // Use the standalone draw path: the account scene has not been bound yet.
    const auto draw = [&]() noexcept {
        if (!standalone_title_layout(&g_probe.shell_scene,
                                     static_cast<float>(renderer.width) * 96 / renderer.dpi,
                                     static_cast<float>(renderer.height) * 96 / renderer.dpi) ||
            !renderer_bind_ui_scene(&renderer, &g_probe.shell_scene))
            return false;
        const float clear[]{0, 0, 0, 1};
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.surface.target, clear);
        return d3d11_renderer_draw_stream(&renderer, kDefaultTheme, true, true, nullptr, 10000);
    };
    // Exercise a DPI change before any visible title, then first draw and reuse.
    const bool pending = renderer.glyph_text.title.run_count == 0;
    renderer_set_dpi(&renderer, 144);
    if (pending && (renderer.glyph_text.title.run_count || renderer.glyph_text.title_quad_count ||
                    renderer.glyph_text.cache_builds || renderer.glyph_text.gpu.dispatch_count))
        return false;
    if (!renderer.glyph_text.ready || !draw() || !renderer.glyph_text.title_quad_count ||
        !probe_capture_frame(L"bootstrap-144.bmp"))
        return false;
    const u32 builds = renderer.glyph_text.cache_builds;
    if (!draw() || renderer.glyph_text.cache_builds != builds)
        return false;
    renderer_set_dpi(&renderer, 192);
    if (!renderer.glyph_text.ready || !draw() || !renderer.glyph_text.title_quad_count ||
        !probe_capture_frame(L"bootstrap-192.bmp"))
        return false;
    const u32 builds_192 = renderer.glyph_text.cache_builds;
    if (!draw() || renderer.glyph_text.cache_builds != builds_192)
        return false;
    renderer_set_dpi(&renderer, 96);
    return renderer.glyph_text.ready && draw() && probe_capture_frame(L"bootstrap-96.bmp");
}

namespace {

// The backed rows of a paged glyph surface (`texel` bytes a texel) into `pixels`, through a staging copy of each
// page's committed rows (never an unbacked tile).
bool read_glyph_surface(const GpuPagedTexture* surface, u32 texel, u8* pixels) noexcept {
    auto& r = g_probe.renderer;
    const GlyphAtlasAllocator& atlas = r.glyph_text.allocator;
    D3D11_TEXTURE2D_DESC description{};
    surface->texture->GetDesc(&description);
    description.BindFlags = description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(r.graphics.device->CreateTexture2D(&description, nullptr, &staging)))
        return false;
    for (u32 page = 0; page < atlas.page_count; ++page) {
        const u32 top = page * atlas.page_rows, rows = atlas.pages[page].committed;
        const D3D11_BOX box{0, top, 0, atlas.width, top + rows, 1};
        if (rows)
            r.graphics.context->CopySubresourceRegion(staging, 0, 0, top, 0, surface->texture, 0, &box);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const bool ok = SUCCEEDED(r.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    for (u32 y = 0; ok && y < atlas.height; ++y)
        copy_bytes(pixels + y * atlas.width * texel, static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch,
                   atlas.width * texel);
    if (ok)
        r.graphics.context->Unmap(staging, 0);
    staging->Release();
    return ok;
}

// Glyphs the atlas holds with no coverage when it is read back (mono glyphs in the R8 surface, colour ones in the
// RGBA surface's alpha); ~0u when the readback failed.
u32 blank_glyphs(const char* stage) noexcept {
    auto& r = g_probe.renderer;
    const GlyphAtlasAllocator& atlas = r.glyph_text.allocator;
    const GpuPagedTexture* color = d3d11_glyph_text_color_surface(r.glyph_text);
    const usize texels = usize{atlas.width} * atlas.height;
    auto* mono = static_cast<u8*>(VirtualAlloc(nullptr, texels * 5, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    u8* rgba = mono ? mono + texels : nullptr;
    const bool ok = mono && read_glyph_surface(r.glyph_text.gpu.surface, 1, mono) &&
                    (!color || read_glyph_surface(color, 4, rgba));
    u32 glyphs = 0, blank = 0;
    for (u32 index = 0; ok && index < atlas.entry_capacity; ++index) {
        const GlyphAtlasEntry& entry = atlas.entries[index];
        if (!entry.occupied)
            continue;
        bool covered = false;
        for (u32 y = entry.y; !covered && y < u32{entry.y} + entry.height; ++y)
            for (u32 x = entry.x; !covered && x < u32{entry.x} + entry.width; ++x)
                covered = mono[y * atlas.width + x] != 0 || (color && rgba[(y * atlas.width + x) * 4 + 3] != 0);
        ++glyphs;
        blank += !covered;
    }
    if (mono)
        VirtualFree(mono, 0, MEM_RELEASE);
    LogEvent(stage).field("glyphs", glyphs).field("blank", blank).field("bytes",
        gpu_paged_texture_bytes(r.glyph_text.gpu.surface)).write(blank || !ok ? LogLevel::error : LogLevel::info,
                                                                 "probe");
    return ok && glyphs ? blank : ~0u;
}

// The glyph quads of the frame drawn now (sign-in title and form text) that are stale (their atlas corner is no
// resident glyph's) or draw no ink on the target (every pixel of the quad as its padding corner).
u32 undrawn_quads(const char* stage) noexcept {
    auto& r = g_probe.renderer;
    const GlyphAtlasAllocator& atlas = r.glyph_text.allocator;
    ProbeTargetReadback image;
    if (!d3d11_renderer_draw_stream(&r, kDefaultTheme, true, true, nullptr) || probe_target_readback(r, &image))
        return ~0u;
    const float scale = static_cast<float>(r.dpi) / 96.0f;
    const auto pixel = [&](u32 x, u32 y) noexcept {
        return static_cast<const u8*>(image.mapped.pData) + y * image.mapped.RowPitch + x * 4;
    };
    u32 quads = 0, stale = 0, undrawn = 0;
    const auto check = [&](const GlyphQuad* list, u32 count) noexcept {
        for (u32 i = 0; list && i < count; ++i) {
            const GlyphQuad& q = list[i];
            if (!(q.u0 >= 0 && q.u0 < 1))
                continue;
            ++quads;
            const u32 x = static_cast<u32>(q.u0 * atlas.width + .5f), y = static_cast<u32>(q.v0 * atlas.height + .5f);
            bool resident = false;
            for (u32 j = 0; !resident && j < atlas.entry_capacity; ++j)
                resident = atlas.entries[j].occupied && atlas.entries[j].x == x && atlas.entries[j].y == y;
            stale += !resident;
            const u32 left = static_cast<u32>(maximum(0.0f, q.left * scale));
            const u32 top = r.caption_height + static_cast<u32>(maximum(0.0f, q.top * scale));
            const u32 right = minimum(image.width, static_cast<u32>(maximum(0.0f, q.right * scale)));
            const u32 bottom =
                minimum(image.height, r.caption_height + static_cast<u32>(maximum(0.0f, q.bottom * scale)));
            if (left >= right || top >= bottom)
                continue;
            const u8* corner = pixel(left, top);
            bool ink = false;
            for (u32 py = top; !ink && py < bottom; ++py)
                for (u32 px = left; !ink && px < right; ++px) {
                    const u8* p = pixel(px, py);
                    for (u32 c = 0; c < 3; ++c)
                        ink = ink || (p[c] > corner[c] ? p[c] - corner[c] : corner[c] - p[c]) > 32;
                }
            undrawn += !ink;
        }
    };
    check(r.glyph_text.title_quads, r.glyph_text.title_quad_count);
    if (r.content_text)
        check(r.content_text->quads, r.content_text->quad_count);
    LogEvent(stage).field("quads", quads).field("stale", stale).field("undrawn", undrawn).write(
        stale || undrawn || !quads ? LogLevel::error : LogLevel::info, "probe");
    return quads ? stale + undrawn : ~0u;
}

RendererLoginView probe_login_view(RendererLoginStatus status, bool visible) noexcept {
    return RendererLoginView{{}, {}, {}, 0, 0, false, LoginControl::none, status, login_mfa_capability_none,
                             login_mfa_capability_none, 0, false, nullptr, false, false, false, visible};
}

// A presented frame whose resident glyphs all have coverage and whose quads are all resident and drawn.
bool glyph_frame(const char* atlas_stage, const char* quad_stage) noexcept {
    return probe_present(&g_probe.renderer, kDefaultTheme) && blank_glyphs(atlas_stage) == 0 &&
           undrawn_quads(quad_stage) == 0;
}

// The main loop's first turn of media and glyph maintenance, which keeps every glyph, then an idle trim past the
// delay, which keeps what the sign-in screen draws; a checked frame after each.
bool glyph_maintenance(const char* loop_atlas, const char* loop_quads, const char* idle_atlas,
                       const char* idle_quads) noexcept {
    auto& r = g_probe.renderer;
    const u64 entries = r.glyph_text.allocator.entry_count;
    renderer_media_maintain(&r, clock_monotonic_milliseconds());
    if (r.glyph_text.allocator.entry_count != entries || !glyph_frame(loop_atlas, loop_quads))
        return false;
    const u32 evicted = r.glyph_text.allocator.evicted_entries;
    renderer_media_maintain(&r, clock_monotonic_milliseconds() + kRendererGlyphIdleTrimMs + kRendererGlyphProtectMs);
    LogEvent(idle_atlas).field("evicted", r.glyph_text.allocator.evicted_entries - evicted)
        .field("entries", r.glyph_text.allocator.entry_count).write(LogLevel::info, "probe");
    return r.glyph_text.allocator.entry_count && glyph_frame(idle_atlas, idle_quads);
}

} // namespace

// The sign-in screen as the client starts it: the first frame under the caption bar (the glyph atlas just adopted
// from the device prestart), the window shown (the caption turns active, the form takes focus and is laid out again),
// the main loop's first turn of media and glyph maintenance, then an idle trim on a screen nothing builds again.
// After each frame every glyph the atlas holds reads back with coverage, and every sign-in quad names a resident
// glyph and draws ink on the target. The first maintenance once trimmed every glyph (the atlas clock was never set),
// and an idle trim evicted the glyphs of the retained sign-in quads, leaving the screen blank.
bool probe_startup_glyph_atlas() noexcept {
    auto& r = g_probe.renderer;
    const auto frame = glyph_frame;
    const u32 caption = static_cast<u32>(ui_caption_bar_height(kDefaultTheme) * r.dpi / 96.0f + .5f);
    renderer_set_caption_height(&r, caption);
    const UiCaptionBarState signin{UiCaptionPart::none, UiCaptionPart::none, false, false, UiCaptionUpdate::hidden,
                                   true, 1};
    if (r.glyph_text.cache_builds || !renderer_resize(&r, r.width, r.height - caption) ||
        !renderer_set_caption(&r, signin, utf8_literal("MaiCord")) ||
        !set_probe_login_view(probe_login_view(RendererLoginStatus::idle, true)) || !r.glyph_text.cache_builds ||
        !frame("startup_glyph_atlas_first", "startup_glyph_quads_first"))
        return false;
    UiCaptionBarState shown = signin;
    shown.active = true;
    RendererLoginView focused = probe_login_view(RendererLoginStatus::idle, true);
    focused.focus = LoginControl::login;
    if (!renderer_set_caption(&r, shown, utf8_literal("MaiCord")) || !set_probe_login_view(focused) ||
        !set_probe_login_view(focused) || !frame("startup_glyph_atlas_shown", "startup_glyph_quads_shown"))
        return false;
    return glyph_maintenance("startup_glyph_atlas_loop", "startup_glyph_quads_loop", "startup_glyph_atlas_idle",
                             "startup_glyph_quads_idle");
}

// The full app shell (guilds, channels, a page of varied messages, the member list) fills the glyph atlas; then the
// account signs out as ui_runtime.cpp releases it (message presentation, content text and navigation unbound) and the
// sign-in screen takes its place, followed by the main loop's maintenance and an idle trim. Every frame's sign-in
// quads are resident and drawn, and every resident glyph reads back with coverage.
bool probe_signout_glyph_atlas() noexcept {
    auto& r = g_probe.renderer;
    static constexpr const char* kLines[]{
        "The quick brown fox jumps over the lazy dog 0123456789",
        "PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS!?",
        "Sphinx of black quartz, judge my vow: (a+b)*[c-d]/{e%f}",
        "**Bold** _italic_ `code` ~~strike~~ #general @Mai",
        "Mr. Jock, TV quiz PhD, bags few lynx; 2024-11-07 13:45 #atlas-debug",
        "Glib jocks quiz nymph to vex dwarf | ^ ~ $ \\ ' \" ; : , .",
        "Jackdaws love my big sphinx of quartz; waltz, bad nymph, for quick jigs vex",
        "How vexingly quick daft zebras jump & 42 > 17 < 99 = true",
    };
    for (u32 i = 0; i < sizeof(kLines) / sizeof(kLines[0]); ++i) {
        u32 bytes = 0;
        while (kLines[i][bytes])
            ++bytes;
        if (!add_message(1'270'000'000'000'000'100ull + i, Utf8View{kLines[i], bytes}))
            return false;
    }
    if (!message_view_runtime_reindex(&g_probe.message_view,
                                      ui_extent_units(kUiBootstrapMessageRowHeight), 1, true)
             .ok())
        return false;
    // The shell at several scales (the window moved across monitors) fills several bands of the atlas.
    static constexpr u32 kDpis[]{96, 144, 192, 240};
    for (const u32 dpi : kDpis) {
        renderer_set_dpi(&r, dpi);
        if (!synchronize_probe_shell_layout() || !rebuild_probe_content_text() ||
            !probe_present(&r, kDefaultTheme) || blank_glyphs("signout_glyph_atlas_shell") != 0)
            return false;
    }
    const u64 shell_bytes = gpu_paged_texture_bytes(r.glyph_text.gpu.surface);
    log_u64(LogLevel::info, "probe", "signout_glyph_atlas_shell_bytes", shell_bytes);
    if (shell_bytes < 2 * 256 * 1024)
        return false;
    renderer_unbind_message_text_presentation(&r);
    renderer_unbind_text_storage(&r);
    renderer_unbind_navigation_view(&r);
    if (!set_probe_login_view(probe_login_view(RendererLoginStatus::signed_out, true)) ||
        !glyph_frame("signout_glyph_atlas_signin", "signout_glyph_quads_signin"))
        return false;
    return glyph_maintenance("signout_glyph_atlas_loop", "signout_glyph_quads_loop", "signout_glyph_atlas_idle",
                             "signout_glyph_quads_idle");
}

bool probe_long_embed_text() noexcept {
    constexpr u64 id = 1'270'000'000'000'000'000ull;
    const auto substitutions = g_probe.text.substitutions;
    constexpr u32 counts[]{150, 300};
    for (u32 count : counts) {
        char source[4096]{};
        ProtocolWriter writer{{reinterpret_cast<u8*>(source), sizeof(source)}, 0};
        for (u32 i = 0; i < count; ++i) {
            if (!protocol_writer_append_byte(&writer, '`') ||
                !protocol_writer_append_u64(&writer, 3'308'051u + i * 19'741u) ||
                !protocol_writer_append_literal(&writer, "`" ) ||
                (i + 1 < count && !protocol_writer_append_literal(&writer, ", "))) return false;
        }
        if (!message_timeline_reset(&g_probe.message_view.timeline) ||
            !add_message(id, utf8_literal("Thanks, see you there :heart:")) ||
            !message_view_runtime_reindex(&g_probe.message_view, ui_extent_units(57), 1, false).ok()) return false;
        const auto found = message_store_find(g_probe.message_view.store, id);
        const auto* record = found.ok() ? message_store_resolve(g_probe.message_view.store, found.handle) : nullptr;
        GatewayMessageEmbed embed{};
        embed.type = GatewayMessageEmbedType::rich;
        embed.title = utf8_literal("Wren042 rolled 150d10000000!");
        embed.description = {source, writer.used};
        embed.has_color = true; embed.color = 0x5865F2;
        GatewayMessageEvent event{}; event.id = id; event.fields = gateway_message_field_embeds;
        event.embeds = {&embed, 1};
        if (!record || !message_rich_store_apply(&g_probe.rich_store, id, record->revision, event).ok() ||
            !synchronize_probe_shell_layout() || !rebuild_probe_content_text() ||
            g_probe.text.substitutions != substitutions) return false;
        bool found_description = false, found_heart = false;
        for (u32 i = 0; i < g_probe.message_text.command_count; ++i) {
            const auto& command = g_probe.message_text.commands[i];
            // Plain runs between the code spans carry size-only spans, so count the code spans.
            u32 code_spans = 0;
            for (u32 span = 0; span < command.styles.count; ++span)
                code_spans += (command.styles[span].flags & (text_style_monospace | text_style_code_block)) != 0;
            found_description = found_description || (code_spans == count && command.text.bytes > count * 8 &&
                command.measured_height > 100);
            found_heart = found_heart || (command.text.bytes == utf8_literal("Thanks, see you there \342\235\244\357\270\217").bytes);
        }
        if (!found_description || !found_heart ||
            !probe_capture_frame(count == 150 ? L"embed-150-code-numbers.bmp" : L"embed-300-code-numbers.bmp")) return false;
    }
    log_write(LogLevel::info, "probe", "long_embed_150_and_300_inline_code_without_cache_substitution_passed");
    return true;
}

// A title-link embed whose description is a ```ansi block, at 96, 144 and 192
// DPI: the accent is the card's leading border, coloured inside both rounded
// left corners and absent outside them, and the code block draws its framed
// box. A code block adds no draw call to an embed.
bool probe_embed_code_block() noexcept {
    constexpr u64 id = 1'270'000'000'000'000'100ull;
    if (!message_timeline_reset(&g_probe.message_view.timeline) || !add_message(id, utf8_literal("x")) ||
        !message_view_runtime_reindex(&g_probe.message_view, ui_extent_units(57), 1, false).ok())
        return false;
    const auto found = message_store_find(g_probe.message_view.store, id);
    const auto* record = found.ok() ? message_store_resolve(g_probe.message_view.store, found.handle) : nullptr;
    GatewayMessageEmbed embed{};
    embed.type = GatewayMessageEmbedType::rich;
    embed.title = utf8_literal("Example Colored Text Tool");
    embed.url = utf8_literal("https://colors.example.com/");
    embed.description =
        utf8_literal("```ansi\nSimple app to make \x1b[1;31mcolored\x1b[0m \x1b[45;37mtext\x1b[0m \x1b[4;34mlike this\x1b[0m!\n```");
    embed.has_color = true;
    embed.color = 0xFF0000;
    GatewayMessageEvent event{};
    event.id = id;
    event.fields = gateway_message_field_embeds;
    event.embeds = {&embed, 1};
    const auto apply = [&]() noexcept {
        return record && message_rich_store_apply(&g_probe.rich_store, id, record->revision, event).ok() &&
               synchronize_probe_shell_layout() && rebuild_probe_content_text() && probe_prepare_capture();
    };
    const auto draws = [&](u32* calls) noexcept {
        u8 pixel[1][4]{};
        const UiPoint origin{1, 1};
        *calls = 0;
        if (!probe_read_pixels(kDefaultTheme, {&origin, 1}, pixel))
            return false;
        *calls = g_probe.renderer.draw_calls_last_frame;
        return true;
    };
    const auto accent = [](const u8* p) noexcept { return p[2] > 200 && p[1] < 80 && p[0] < 80; };
    const auto reddish = [](const u8* p) noexcept { return p[2] > 120 && p[1] < 110 && p[0] < 110; };
    const u32 original_dpi = g_probe.renderer.dpi;
    constexpr u32 dpis[]{96, 144, 192};
    bool valid = apply();
    for (u32 dpi : dpis) {
        renderer_set_dpi(&g_probe.renderer, dpi);
        valid = valid && synchronize_probe_shell_layout() && rebuild_probe_content_text() && probe_prepare_capture();
        const MessageSceneRowWidget* row = nullptr;
        for (u32 i = 0; valid && i < g_probe.message_scene.count; ++i)
            if (g_probe.message_scene.rows[i].message_id == id)
                row = &g_probe.message_scene.rows[i];
        const UiLayoutNode* item =
            row ? message_scene_rich_item_layout_node(g_probe.message_scene, *row, MessageSceneRichItemKind::embed, 0)
                : nullptr;
        UiRect box{};
        for (u32 i = 0; i < g_probe.message_text.command_count; ++i)
            if (g_probe.message_text.commands[i].message_id == id &&
                message_text_command_code_block(g_probe.message_text.commands[i]))
                box = message_text_code_block_box(g_probe.message_text.commands[i]);
        if (!item || !ui_rect_has_area(box)) {
            valid = false;
            break;
        }
        const float measured = row->rich_item_layout.embed_widths[0];
        const float l = item->rectangle.left, t = item->rectangle.top;
        const float r = minimum(item->rectangle.right, l + (measured > 0 ? measured : kMessageEmbedMaximumWidth));
        const float b = item->rectangle.bottom - kMessageEmbedItemGap, middle = (t + b) * .5f;
        // Pixel centres stay within half a DIP of these points at every scale.
        const UiPoint points[]{{l + 2, middle},       {l + 2.2f, t + 2},    {l + 2.2f, b - 2},
                               {l + .3f, t + .3f},    {l + .3f, b - .3f},   {l + 6, middle},
                               {box.left + 3, box.top + 3}, {box.left + 3, box.top - 3}};
        u8 pixels[8][4]{};
        const bool read = probe_read_pixels(kDefaultTheme, {points, 8}, pixels);
        const bool corners = read && accent(pixels[0]) && accent(pixels[1]) && accent(pixels[2]) &&
                             !reddish(pixels[3]) && !reddish(pixels[4]) && !reddish(pixels[5]);
        // The box spans the card's text column and differs from the card's fill.
        const bool boxed = read && box.right > r - kMessageEmbedEndInset - .5f &&
                           box.right < r - kMessageEmbedEndInset + .5f &&
                           (pixels[6][0] != pixels[7][0] || pixels[6][1] != pixels[7][1] ||
                            pixels[6][2] != pixels[7][2]);
        log_u64(corners ? LogLevel::info : LogLevel::error, "probe", "embed_accent_corners_dpi", dpi);
        log_u64(boxed ? LogLevel::info : LogLevel::error, "probe", "embed_code_box_dpi", dpi);
        wchar_t capture[] = L"embed-code-block-000.bmp";
        capture[17] = static_cast<wchar_t>(L'0' + dpi / 100);
        capture[18] = static_cast<wchar_t>(L'0' + dpi / 10 % 10);
        capture[19] = static_cast<wchar_t>(L'0' + dpi % 10);
        valid = valid && corners && boxed && probe_capture_frame(capture);
    }
    renderer_set_dpi(&g_probe.renderer, original_dpi);
    u32 with_code = 0, with_text = 0, without = 0;
    valid = valid && apply() && draws(&with_code);
    embed.description = utf8_literal("Simple app to make colored text like this!");
    valid = valid && apply() && draws(&with_text);
    event.embeds = {};
    valid = valid && apply() && draws(&without);
    log_u64(LogLevel::info, "probe", "embed_code_block_draw_calls", with_code);
    log_u64(LogLevel::info, "probe", "embed_text_draw_calls", with_text);
    log_u64(LogLevel::info, "probe", "no_embed_draw_calls", without);
    valid = valid && with_code == with_text && synchronize_probe_shell_layout() && rebuild_probe_content_text();
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "embed_code_block_accent_and_box_passed" : "embed_code_block_accent_and_box_failed");
    return valid;
}

} // namespace ui_probe_detail
} // namespace maicord
