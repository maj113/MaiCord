#pragma once

#include "probe_present.hpp"
#include "maicord/ui/pinned_messages_avatar_media.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/surface_geometry.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "maicord/core/log.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

namespace maicord {
namespace pinned_messages_probe_detail {
struct Source { u32 calls{}; bool current = true; };
inline bool current(void* context) noexcept { return static_cast<Source*>(context)->current; }
inline bool resolve(void* context, u32 index, UiPinnedMessagesRow* row) noexcept {
    ++static_cast<Source*>(context)->calls;
    *row = {71, 1000 + index, 9000 + index, utf8_literal("Original author"),
            utf8_literal("2026-10-05"),
            utf8_literal("This original pinned message keeps its source identity when you jump back to the conversation."),
            utf8_literal("Pinned message"), utf8_literal("0123456789abcdef0123456789abcdef"),
            48, true, false};
    return true;
}
} // namespace pinned_messages_probe_detail

inline bool pinned_messages_ui_probe(D3D11Renderer& renderer,
                                     bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!renderer.ui_scene || !renderer.content_text || !renderer.dpi)
        return false;
    UiPinnedMessagesScene scene{};
    pinned_messages_probe_detail::Source fixture{};
    UiPinnedMessagesSource source{&fixture, 50, 1, pinned_messages_probe_detail::resolve,
                                 pinned_messages_probe_detail::current};
    UiPinnedMessagesContent content{};
    content.channel = utf8_literal("# release-planning");
    content.channel_id = 71;
    content.revision = 1;
    content.phase = UiPinnedMessagesPhase::ready;
    content.has_next = true;
    struct Restore {
        D3D11Renderer& renderer;
        UiPinnedMessagesScene& scene;
        const UiPinnedMessagesScene* pins;
        const UiWidgetManager* focus;
        ~Restore() noexcept {
            renderer.pinned_messages_scene = pins;
            renderer.focus_owner = focus;
            renderer.interaction_text_pinned_messages_revision = ~0u;
            renderer.retained_presentation.valid = false;
            content_text_retention_invalidate(&renderer.content_text_retention);
            ui_pinned_messages_scene_release(&scene);
        }
    } restore{renderer, scene, renderer.pinned_messages_scene, renderer.focus_owner};
    const u32 original_width = renderer.width, original_height = renderer.height;
    const auto show = [&](const wchar_t* name) noexcept {
        auto viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
        viewport.top = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::title_bar).bottom;
        viewport.bottom = minimum(viewport.bottom,
            ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::message_view).bottom);
        viewport.right = static_cast<float>(renderer.width) * 96.f / maximum(renderer.dpi, 1u);
        const auto header = ui_scene_header_geometry(*renderer.ui_scene);
        const auto* pins = ui_scene_resolve(*renderer.ui_scene,
                                           ui_scene_find(*renderer.ui_scene, UiSemantic::pins_toggle));
        if (!pins || !pins->visible || !ui_rect_has_area(header.pins) ||
            !ui_pinned_messages_scene_update(&scene, &source, &content, viewport) ||
            scene.panel.right != viewport.right || scene.panel.top != viewport.top ||
            scene.panel.bottom != viewport.bottom)
            return false;
        renderer.pinned_messages_scene = &scene;
        renderer.focus_owner = &scene.widgets;
        renderer.retained_presentation.valid = false;
        if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
            !probe_present(&renderer, kDefaultTheme))
            return false;
        return !capture || capture(name);
    };
    if (!show(L"pinned-messages-ready.bmp"))
        return false;
    PinnedMessagesAvatarMedia avatar{};
    if (!scene.count || scene.count > kUiPinnedMessagesVisibleRows ||
        !pinned_messages_avatar_media_resolve(scene, 0, &avatar) ||
        !pinned_messages_avatar_media_current(scene, avatar.key))
        return false;
    // Populate the existing atlas with one synthetic transparent avatar. No
    // network request, private asset, second atlas or new decoder is involved.
    u8 pixels[16 * 16 * 4]{};
    for (u32 y = 8; y < 16; ++y) for (u32 x = 0; x < 16; ++x) {
        auto* pixel = pixels + (y * 16 + x) * 4;
        pixel[0] = 220; pixel[1] = 80; pixel[2] = 120; pixel[3] = 255;
    }
    const u32 media_before = renderer.media_quad_count;
    if (!renderer_upload_media_rgba8(&renderer, avatar.key, 16, 16, 64,
                                      {pixels, sizeof(pixels)}).ok() ||
        !show(L"pinned-messages-resident-avatar.bmp") || renderer.media_quad_count <= media_before)
        return false;
    u32 cleared = 0;
    for (u32 i = 0; i < renderer.paint_stream.count; ++i) {
        const auto& q = renderer.paint_stream.quads[i];
        const UiRect r{q.left, q.top, q.right, q.bottom};
        const auto a = avatar.rectangle;
        if (q.border_width >= 0 && (ui_rect_equal(r, a) ||
            ui_rect_equal(r, {a.left + 10, a.top + 6, a.right - 10, a.top + 14}) ||
            ui_rect_equal(r, {a.left + 6, a.top + 16, a.right - 6, a.bottom - 4}))) {
            if (q.fill.alpha != 0 || q.border.alpha != 0) return false;
            ++cleared;
        }
    }
    if (cleared != 3) return false;
    log_write(LogLevel::info, "probe", "pinned_resident_avatar_placeholder_clear_passed");
    const u32 calls = fixture.calls, layout = scene.widgets.layout.revision,
              builds = renderer.content_text->rebuilds, raster = renderer.glyph_text.cache_builds,
              paint_uploads = renderer.paint_instance_uploads, text_uploads = renderer.text_instance_uploads,
              media_uploads = renderer.media_instance_uploads, painted = renderer.painted_frames,
              skips = renderer.unchanged_present_skips;
    for (u32 frame = 0; frame < 64; ++frame) {
        if (!ui_pinned_messages_scene_update(&scene, &source, &content, scene.viewport) ||
            !renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
            !probe_present(&renderer, kDefaultTheme))
            return false;
    }
    if (fixture.calls != calls || scene.widgets.layout.revision != layout ||
        renderer.content_text->rebuilds != builds || renderer.glyph_text.cache_builds != raster ||
        renderer.paint_instance_uploads != paint_uploads || renderer.text_instance_uploads != text_uploads ||
        renderer.media_instance_uploads != media_uploads || renderer.painted_frames != painted ||
        renderer.unchanged_present_skips - skips != 64)
        return false;
    log_u64(LogLevel::info, "probe", "pinned_retained_frames", 64);
    log_write(LogLevel::info, "probe", "pinned_zero_resolve_layout_shape_raster_upload_draw_passed");
    if (!ui_pinned_messages_scene_scroll(&scene, minimum(180.f, ui_pinned_messages_scene_scroll_maximum(scene))) ||
        !renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
        !probe_present(&renderer, kDefaultTheme) ||
        (capture && !capture(L"pinned-messages-scrolled.bmp")))
        return false;
    const auto visible = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    fixture.current = false;
    const auto stale = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    if (d3d11_renderer_pinned_messages_visible(renderer) ||
        pinned_messages_avatar_media_current(scene, avatar.key) ||
        renderer_presentation_stamp_equal(visible, stale) ||
        !renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
        !probe_present(&renderer, kDefaultTheme))
        return false;
    fixture.current = true;
    ++source.revision;
    source.count = 0;
    content.phase = UiPinnedMessagesPhase::loading;
    ++content.revision;
    if (!show(L"pinned-messages-loading.bmp"))
        return false;
    content.phase = UiPinnedMessagesPhase::failed;
    content.status = utf8_literal("Could not load pinned messages. Try again.");
    ++content.revision;
    if (!show(L"pinned-messages-retry.bmp"))
        return false;
    content.phase = UiPinnedMessagesPhase::empty;
    content.status = {};
    content.has_next = false;
    ++content.revision;
    if (!show(L"pinned-messages-empty.bmp"))
        return false;
    source.count = 50;
    ++source.revision;
    content.phase = UiPinnedMessagesPhase::ready;
    content.has_next = true;
    content.status = {};
    ++content.revision;
    const auto viewport = scene.viewport;
    const float outside = renderer.width * 96.f / renderer.dpi - ui_rect_width(viewport);
    const u32 compact_width = static_cast<u32>((outside + 360.f) * renderer.dpi / 96.f);
    if (!probe_resize_surface(compact_width, renderer.height) ||
        !show(L"pinned-messages-narrow.bmp") || !scene.overlay)
        return false;
    const auto* composer = renderer.message_composer_scene;
    const auto content_width = ui_rect_width(ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content));
    if (!composer || !composer->active || content_width < kUiShellMinimumMessageWidthDips ||
        ui_rect_width(composer->viewport) < kUiShellMinimumMessageWidthDips - 2 * kUiFloatingSurfaceInset)
        return false;
    const auto* surface = ui_widget_manager_layout_node(composer->widgets, composer->surface);
    if (!surface || !ui_rect_has_area(surface->rectangle)) return false;
    // At this width the GIF and sticker buttons fold into the emoji picker.
    const MessageComposerControl controls[]{MessageComposerControl::add_attachment, MessageComposerControl::edit,
        MessageComposerControl::emoji, MessageComposerControl::send};
    for (const auto control : controls) {
        const auto* node = message_composer_scene_layout_node(*composer, control);
        if (!node || !ui_rect_has_area(node->rectangle) ||
            !ui_rect_equal(node->rectangle, ui_rect_intersection(node->rectangle, surface->rectangle)) ||
            !ui_rect_equal(node->rectangle, ui_rect_intersection(node->rectangle, node->clip_rectangle)))
            return false;
    }
    if (!probe_resize_surface(original_width, original_height))
        return false;
    log_write(LogLevel::info, "probe", "pinned_current_authority_clipped_scroll_states_narrow_passed");
    return true;
}
} // namespace maicord
