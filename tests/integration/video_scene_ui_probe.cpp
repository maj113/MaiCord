#include "video_scene_ui_probe.hpp"
#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
namespace {
u64 prefix_signature(const D3D11Renderer&) noexcept;
bool prefix_cursors_equal(const RendererTextPrefix&, const RendererTextPrefix&) noexcept;
bool capture_native_video_preview(D3D11Renderer& renderer, UiVideoScene& scene,
                                  bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture)
        return true;
    u32 failed_stage = 0;
    const auto require = [&](bool accepted, u32 stage) noexcept {
        if (!accepted && !failed_stage) {
            failed_stage = stage;
            log_u64(LogLevel::error, "probe", "video_native_preview_failure_stage", stage);
        }
        return accepted;
    };
    struct Fixture {
        MessageRecord record[1]{};
        u32 order[1]{};
        u8 text[64]{};
        MessageRichRecord rich_record[1]{};
        u16 rich_order[1]{};
        u8 rich_text[1024]{};
        u32 heights[1]{};
        u64 fenwick[1]{};
        MessageSceneRowWidget rows[1]{};
        UiWidget widgets[kMessageSceneWidgetsPerRow + 1]{};
        UiLayoutNode nodes[kMessageSceneWidgetsPerRow + 1]{};
        MessageStore store{};
        MessageRichStore rich{};
        UiVariableList list{};
        MessageTimeline timeline{};
        MessageScene messages{};
        MessageTextPresentation message_text{};
    } fixture;
    constexpr u64 message_id = 0x564944454f;
    constexpr u64 attachment_id = 0x504f53544552;
    bool valid = require(
        message_store_initialize(&fixture.store, {fixture.record, 1}, {fixture.order, 1},
                                 {fixture.text, 64}) &&
            message_store_upsert(&fixture.store, {message_id, 7, 9, 0, 0, utf8_literal(""), 0}).ok() &&
            message_rich_store_initialize(&fixture.rich, {fixture.rich_record, 1}, {fixture.rich_order, 1},
                                          {fixture.rich_text, 1024}) &&
            ui_variable_list_initialize(&fixture.list, {fixture.heights, 1}, {fixture.fenwick, 1}, 1,
                                        ui_extent_units(57)) &&
            message_timeline_initialize(&fixture.timeline, &fixture.store, &fixture.list,
                                        ui_extent_units(57), ui_extent_units(1000), false) &&
            message_timeline_bind_rich_store(&fixture.timeline, &fixture.rich) &&
            message_scene_initialize(&fixture.messages, &fixture.timeline, {fixture.rows, 1},
                                     {fixture.widgets, kMessageSceneWidgetsPerRow + 1},
                                     {fixture.nodes, kMessageSceneWidgetsPerRow + 1}),
        1);
    GatewayMessageAttachment attachment{};
    attachment.id = attachment_id;
    attachment.filename = utf8_literal("native-preview.mp4");
    attachment.content_type = utf8_literal("video/mp4");
    attachment.url = utf8_literal("https://example.com/native-preview.mp4");
    attachment.width = 256;
    attachment.height = 170;
    attachment.has_width = attachment.has_height = true;
    GatewayMessageEvent event{};
    event.id = message_id;
    event.fields = gateway_message_field_attachments;
    event.attachments = {&attachment, 1};
    // The native renderer validates this viewport against the real shell.
    // Positioning a fixture by changing the viewport would violate that
    // contract rather than moving a row inside the message pane.
    const UiRect viewport = renderer.message_scene->viewport;
    valid =
        valid &&
        require(
            message_rich_store_apply(&fixture.rich, message_id, fixture.record[0].revision, event).ok() &&
                message_scene_update(&fixture.messages, viewport).ok(),
            2);
    // Borrowed-row scenes keep their caller's initial timeline height until
    // measurement is applied. Publish the empty body's metrics together with
    // the estimated attachment layout, then lay out the expanded row again.
    bool metrics_changed = false;
    valid = valid && require(message_scene_set_text_metrics(&fixture.messages, 0, 0, 0, false, nullptr,
                                                            nullptr, {}, &metrics_changed) &&
                                 message_scene_update(&fixture.messages, viewport).ok(),
                             12);
    const auto* node = valid
                           ? message_scene_rich_item_layout_node(fixture.messages, fixture.messages.rows[0],
                                                                 MessageSceneRichItemKind::attachment, 0)
                           : nullptr;
    if (!require(node != nullptr, 3))
        return false;
    float width{}, height{};
    valid = require(
        message_rich_attachment_preview_extent(fixture.rich, fixture.rich_record[0].attachments[0],
                                               ui_rect_width(node->content_rectangle), &width, &height),
        4);
    const UiRect preview{node->rectangle.left, node->rectangle.top, node->rectangle.left + width,
                         node->rectangle.top + height};
    const auto* row = message_scene_layout_node(fixture.messages, fixture.messages.rows[0].widget);
    const UiRect media_clip = row ? ui_rect_intersection(row->rectangle, viewport) : UiRect{};
    if (!require(ui_rect_equal(ui_rect_intersection(preview, media_clip), preview), 13))
        return false;
    const auto request = media_presentation_request(256, 170, width, height, renderer.dpi, 512);
    const auto resolution =
        renderer.preview_media_atlas ? request.resolution : MediaResolutionTier::original;
    const auto key = media_asset_attachment(attachment_id, MediaAttachmentFormat::mp4, resolution);
    // Bounded original checker/landscape colors make transparency and masking
    // visible through the actual native atlas/message-media rendering path.
    u8 pixels[64 * 64 * 4]{};
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            const u32 at = (y * 64 + x) * 4;
            const bool light = ((x / 8) + (y / 8)) % 2 != 0;
            pixels[at] = static_cast<u8>(70 + x * 2 + (light ? 20 : 0));
            pixels[at + 1] = static_cast<u8>(65 + y * 2 + (light ? 15 : 0));
            pixels[at + 2] = static_cast<u8>(150 - y + (light ? 15 : 0));
            pixels[at + 3] = 255;
        }
    }
    // The straight-RGBA uploader routes large-preview identities to their
    // preview atlas. The legacy premultiplied uploader targets only the primary
    // atlas, which the native MP4 paint path does not sample for this key.
    // Every generated pixel is opaque, so both color representations coincide.
    valid = valid && require(renderer_upload_media_straight_rgba8(&renderer, key, 64, 64, 64 * 4,
                                                                  {pixels, sizeof(pixels)})
                                 .ok(),
                             5);
    const auto* original_messages = renderer.message_scene;
    const auto* original_message_text = renderer.message_text;
    const UiVideoScene original_scene = scene;
    renderer.message_scene = &fixture.messages;
    // This fixture deliberately has an empty body. The live text projection
    // belongs to the original rows and must not be composited onto this poster.
    // Keep an active, empty fixture projection to satisfy the native text
    // renderer's real-scene precondition rather than bypassing that check.
    fixture.message_text.active = true;
    fixture.message_text.projection_signature =
        message_text_presentation_projection_signature(fixture.message_text);
    renderer.message_text = &fixture.message_text;
    ui_video_scene_blur(&scene);
    UiVideoPlaybackState playback{message_id, 1,     1,     3000, 16000, 65,   false,
                                  false,      false, false, true, true,  true, true};
    valid = valid &&
            require(ui_video_scene_project(&scene, playback, preview, viewport, {}, {}, kDefaultTheme), 6);
    ui_video_scene_hover(&scene, {(preview.left + preview.right) * .5f, preview.top + 20});
    ui_video_scene_tick(&scene, 0, true);
    renderer.retained_presentation.valid = false;
    valid = valid && require(renderer_rebuild_content_text(&renderer, kDefaultTheme), 7) &&
            require(probe_present(&renderer, kDefaultTheme), 8) &&
            require(renderer.media_quad_count != 0, 9);
    const float center_play_target = ui_rect_width(scene.center_play);
    if (valid) {
        const auto warm_prefix = renderer.content_text_prefix;
        const u32 text_builds = renderer.content_text->rebuilds;
        const u32 text_uploads = renderer.text_instance_uploads;
        const u64 semantic_revision = scene.text_revision;
        const u64 base_signature = prefix_signature(renderer);
        const auto present_fade = [&]() noexcept {
            if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
                !probe_present(&renderer, kDefaultTheme) || renderer.media_quad_count == 0)
                return false;
            return renderer.content_text_prefix.base_builds == warm_prefix.base_builds &&
                   prefix_cursors_equal(renderer.content_text_prefix, warm_prefix) &&
                   prefix_signature(renderer) == base_signature;
        };
        playback.playing = true;
        valid =
            require(ui_video_scene_project(&scene, playback, preview, viewport, {}, {}, kDefaultTheme), 17);
        ui_video_scene_pointer(&scene, UiVideoPointerEvent::leave, {});
        ui_video_scene_tick(&scene, 100, false);
        ui_video_scene_tick(&scene, 220, false);
        valid =
            valid &&
            require(scene.controls_transition.value == 0 && !ui_video_scene_next_wake(scene, 220, false) &&
                        !ui_rect_has_area(scene.center_play) && scene.text_revision == semantic_revision &&
                        present_fade() && capture(L"video-playing-off-hover-native-preview.bmp"),
                    18);
        ui_video_scene_hover(&scene, {(preview.left + preview.right) * .5f, preview.top + 20});
        ui_video_scene_tick(&scene, 240, false);
        ui_video_scene_tick(&scene, 300, false);
        valid = valid && require(scene.controls_transition.value == .5f &&
                                     scene.text_revision == semantic_revision && present_fade() &&
                                     capture(L"video-controls-half-opacity-native-preview.bmp"),
                                 19);
        ui_video_scene_tick(&scene, 360, false);
        // A hovered playing player keeps exactly one wake: its idle deadline.
        valid = valid && require(scene.controls_transition.value == 1 &&
                                     ui_video_scene_next_wake(scene, 360, false) ==
                                         scene.activity_ms + scene.style.idle_ms &&
                                     present_fade() && capture(L"video-controls-compact-native-preview.bmp"),
                                 10);
        if (valid) {
            log_u64(LogLevel::info, "probe", "video_fade_base_builds_delta",
                    renderer.content_text_prefix.base_builds - warm_prefix.base_builds);
            log_u64(LogLevel::info, "probe", "video_fade_overlay_builds_delta",
                    renderer.content_text_prefix.overlay_builds - warm_prefix.overlay_builds);
            log_u64(LogLevel::info, "probe", "video_fade_text_builds_delta",
                    renderer.content_text->rebuilds - text_builds);
            log_u64(LogLevel::info, "probe", "video_fade_text_uploads_delta",
                    renderer.text_instance_uploads - text_uploads);
            log_u64(LogLevel::info, "probe", "video_opacity_transition_bytes", sizeof(UiOpacityTransition));
            log_u64(LogLevel::info, "probe", "video_scene_bytes", sizeof(UiVideoScene));
            log_u64(LogLevel::info, "probe", "video_scene_style_bytes", sizeof(UiVideoSceneStyle));
            log_write(LogLevel::info, "probe", "video_hover_fade_native_preview_passed");
        }
    }
    if (valid) {
        const u64 text_revision = scene.text_revision;
        const u32 text_builds = renderer.content_text->rebuilds;
        playback.buffering = true;
        valid =
            require(ui_video_scene_project(&scene, playback, preview, viewport, {}, {}, kDefaultTheme) &&
                        scene.text_revision == text_revision && !scene.projection.controls[5].visible &&
                        ui_rect_has_area(scene.buffering_indicator) && !ui_rect_has_area(scene.center_play),
                    14) &&
            require(probe_present(&renderer, kDefaultTheme) &&
                        renderer.content_text->rebuilds == text_builds &&
                        capture(L"video-buffering-spinner-phase-0.bmp"),
                    15);
        const u32 paint_builds = renderer.paint_stream_builds;
        valid = valid && require(ui_video_scene_tick(&scene, 256, false) &&
                                     ui_video_scene_next_wake(scene, 256, false) == 288 &&
                                     !ui_video_scene_next_wake(scene, 256, true) &&
                                     probe_present(&renderer, kDefaultTheme) &&
                                     renderer.paint_stream_builds > paint_builds &&
                                     renderer.content_text->rebuilds == text_builds &&
                                     capture(L"video-buffering-spinner-phase-8.bmp"),
                                 16);
        if (valid)
            log_write(LogLevel::info, "probe", "video_buffering_spinner_rotates_without_text_rebuild");
    }
    if (valid && capture) {
        // The media viewer hosts the same scene above its media quad: expanded
        // playback, then an image item of a three-item gallery.
        static UiImageViewer viewer{};
        const float scale = 96.0f / static_cast<float>(renderer.dpi);
        const UiRect window{0, 0, renderer.width * scale, renderer.height * scale};
        const auto* original_viewer = renderer.image_viewer;
        const auto* original_video = renderer.video_scene;
        playback.buffering = false;
        playback.playing = true;
        // In the viewer full screen belongs to its toolbar, not the bar.
        playback.can_expand = false;
        playback.expanded = true;
        playback.elapsed_ms = 7000;
        UiVideoSceneStyle expanded{};
        expanded.preview_radius = 0;
        expanded.idle_ms = kUiVideoControlsExpandedIdleMs;
        expanded.scrim_lead = 40;
        valid = require(ui_image_viewer_set_dpi(&viewer, renderer.dpi) &&
                            ui_image_viewer_open(&viewer, window, {256, 170}) &&
                            ui_image_viewer_set_item(&viewer, {256, 170}, 1, 3, true) &&
                            ui_image_viewer_set_actions(&viewer, ui_image_viewer_action_open) &&
                            ui_image_viewer_set_save_available(&viewer, true) &&
                            ui_video_scene_project(&scene, playback, viewer.display, viewer.viewport, {},
                                                   expanded, kDefaultTheme),
                        20);
        const auto seek = scene.projection.controls[1].bounds;
        ui_video_scene_hover(&scene, {seek.left + ui_rect_width(seek) * .7f, (seek.top + seek.bottom) * .5f});
        ui_video_scene_tick(&scene, 400, true);
        renderer.image_viewer = &viewer;
        renderer.image_viewer_key = key;
        renderer.image_viewer_fallback = key;
        renderer.video_scene = &scene;
        renderer.video_scene_viewer = true;
        renderer.retained_presentation.valid = false;
        valid = valid && require(d3d11_renderer_viewer_video_visible(renderer) &&
                                     renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                                     probe_present(&renderer, kDefaultTheme) &&
                                     capture(L"video-expanded-viewer.bmp"),
                                 21);
        ui_video_scene_focus(&scene, UiVideoControl::volume, false);
        const auto speaker = scene.projection.controls[3].bounds;
        ui_video_scene_hover(&scene, {(speaker.left + speaker.right) * .5f, (speaker.top + speaker.bottom) * .5f});
        valid = valid && require(ui_video_scene_project(&scene, playback, viewer.display, viewer.viewport, {},
                                                        expanded, kDefaultTheme) &&
                                     renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                                     probe_present(&renderer, kDefaultTheme) &&
                                     capture(L"video-expanded-volume.bmp"),
                                 22);
        renderer.video_scene_viewer = false;
        renderer.video_scene = original_video;
        ui_image_viewer_set_actions(&viewer, ui_image_viewer_action_open | ui_image_viewer_action_copy);
        valid = valid && require(ui_image_viewer_set_item(&viewer, {256, 170}, 0, 3, false), 23);
        renderer.retained_presentation.valid = false;
        valid = valid && require(ui_image_viewer_set_caption(&viewer, utf8_literal("A synthetic gallery caption")) &&
                                     renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                                     probe_present(&renderer, kDefaultTheme) &&
                                     capture(L"media-viewer-gallery-image.bmp"),
                                 24);
        // A double click zooms in, then the loading and failed states without a resident image.
        ui_image_viewer_toggle_zoom(&viewer, {(window.left + window.right) * .5f, (window.top + window.bottom) * .5f});
        valid = valid && require(renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                                     probe_present(&renderer, kDefaultTheme) &&
                                     capture(L"media-viewer-double-click-zoom.bmp"),
                                 25);
        ui_image_viewer_apply(&viewer, UiImageViewerCommand::fit);
        renderer.image_viewer_key = renderer.image_viewer_fallback = {};
        valid = valid && require(ui_image_viewer_set_media(&viewer, UiImageViewerMedia::loading) &&
                                     renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                                     probe_present(&renderer, kDefaultTheme) &&
                                     capture(L"media-viewer-loading.bmp"),
                                 26);
        valid = valid && require(ui_image_viewer_set_media(&viewer, UiImageViewerMedia::failed) &&
                                     ui_image_viewer_set_fullscreen(&viewer, true) &&
                                     renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                                     probe_present(&renderer, kDefaultTheme) &&
                                     capture(L"media-viewer-failed.bmp"),
                                 27);
        renderer.image_viewer = original_viewer;
        renderer.video_scene_viewer = false;
        viewer.active = false;
    }
    if (valid) {
        log_u64(LogLevel::info, "probe", "video_controls_single_row_height_dip",

                static_cast<u64>(ui_rect_height(scene.projection.bounds)));
        log_u64(LogLevel::info, "probe", "video_center_play_target_dip",
                static_cast<u64>(center_play_target));
        log_u64(LogLevel::info, "probe", "video_native_generated_preview_pixels", 64 * 64);
    }
    renderer.message_scene = original_messages;
    renderer.message_text = original_message_text;
    scene = original_scene;
    renderer.retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer.content_text_retention);
    const bool restored = require(renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                                      probe_present(&renderer, kDefaultTheme),
                                  11);
    return valid && restored;
}
u64 prefix_signature(const D3D11Renderer& r) noexcept {
    const auto& p = r.content_text_prefix;
    const auto& s = *r.content_text;
    u64 hash = hash64_counted_bytes(kHash64Seed, s.quads, p.quads * sizeof(GlyphQuad));
    hash = hash64_counted_bytes(hash, s.batches, p.batches * sizeof(GlyphDrawBatch));
    hash = hash64_counted_bytes(hash, s.content_semantics, p.semantics * sizeof(MessageTextSemantic));
    hash = hash64_counted_bytes(hash, s.content_hit_regions, p.hits * sizeof(TextHitRegion));
    hash = hash64_counted_bytes(hash, r.text_effects.materials, p.effects * sizeof(TextEffectMaterial));
    return hash64_mix(hash, p.animated);
}
bool prefix_cursors_equal(const RendererTextPrefix& a, const RendererTextPrefix& b) noexcept {
    return a.storage == b.storage && a.message_root.manager == b.message_root.manager &&
           a.message_root.handle.slot == b.message_root.handle.slot &&
           a.message_root.handle.generation == b.message_root.handle.generation && a.quads == b.quads &&
           a.batches == b.batches && a.semantics == b.semantics && a.hits == b.hits &&
           a.effects == b.effects && a.animated == b.animated;
}
} // namespace
bool video_scene_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (renderer.login_visible || !renderer.message_scene || !renderer.content_text) {
        const u64 mask = (renderer.login_visible ? 1ull : 0) | (renderer.message_scene ? 2ull : 0) |
                         (renderer.content_text ? 4ull : 0);
        log_u64(LogLevel::error, "probe", "video_scene_ui_precondition_presence", mask);
        return false;
    }
    UiVideoScene scene{};
    const auto* old = renderer.video_scene;
    struct Restore {
        D3D11Renderer& renderer;
        const UiVideoScene* old;
        ~Restore() noexcept {
            renderer.video_scene = old;
        }
    } restore{renderer, old};
    UiVideoControlsStyle controls{};
    UiVideoSceneStyle style{};
    // Retention/layout fixtures isolate their original invariants from the
    // separate default-duration native fade capture above.
    style.fade_ms = 0;
    UiVideoPlaybackState state{0x564944454f, 1,    1,     372000, 3720000, 65,   true,
                               false,        true, false, true,   true,    true, true};
    const UiRect viewport = renderer.message_scene->viewport;
    const float available = ui_rect_width(viewport) - 2 * controls.padding;
    // The focused fixture has one text-only row at the top. Place injected
    // controls in the blank body area so captured glyphs cannot cross them.
    constexpr float fixture_top_offset = 160;
    if (available < controls.normal_width ||
        ui_rect_height(viewport) <
            fixture_top_offset + ui_video_controls_height(controls, 256) + controls.padding) {
        log_u64(LogLevel::error, "probe", "video_scene_ui_precondition_width_dip_x100",
                static_cast<u64>(maximum(0.f, available) * 100.f));
        log_u64(LogLevel::error, "probe", "video_scene_ui_precondition_height_dip_x100",
                static_cast<u64>(maximum(0.f, ui_rect_height(viewport)) * 100.f));
        log_u64(LogLevel::error, "probe", "video_scene_ui_precondition_dpi", renderer.dpi);
        return false;
    }
    const float left = viewport.left + controls.padding, top = viewport.top + fixture_top_offset;
    UiRect bounds{};
    const auto project = [&](float width) noexcept {
        bounds = {left, top, left + width, top + 180};
        if (!ui_video_scene_project(&scene, state, bounds, viewport, controls, style, kDefaultTheme))
            return false;
        ui_video_scene_hover(&scene, {(bounds.left + bounds.right) * .5f, bounds.top + 20});
        return true;
    };
    // Warm a real message-owned semantic hit range, not an overlay/fake widget.
    // The borrowed command and signature are restored before leaving this probe.
    auto* message_text = const_cast<MessageTextPresentation*>(renderer.message_text);
    MessageTextCommand* semantic_command = nullptr;
    ConstSlice<MessageMarkdownSemantic> original_semantics{};
    const u64 original_signature = message_text ? message_text->projection_signature : 0;
    MessageMarkdownSemantic link{};
    if (message_text)
        for (u32 i = 0; i < message_text->command_count; ++i) {
            auto& command = message_text->commands[i];
            if (command.measurement_kind != MessageTextMeasurementKind::content || !command.text.bytes ||
                command.text.bytes > 65535 ||
                !ui_rect_has_area(ui_rect_intersection(command.clip, viewport)))
                continue;
            semantic_command = &command;
            original_semantics = command.semantics;
            link = {42,
                    0,
                    command.text.bytes,
                    0,
                    static_cast<u16>(command.text.bytes),
                    MessageMarkdownSemanticKind::link,
                    0};
            command.semantics = {&link, 1};
            message_text->projection_signature =
                message_text_presentation_projection_signature(*message_text);
            break;
        }
    renderer.video_scene = &scene;
    u32 stage = __LINE__;
    bool valid = project(minimum(640.f, available)) && d3d11_renderer_video_scene_visible(renderer) &&
                 !scene.projection.compact && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                 probe_present(&renderer, kDefaultTheme) &&
                 renderer.interaction_text_video_revision == ui_video_scene_text_paint_revision(scene) &&
                 (renderer.interaction_text_active_mask & (1u << 9));
    if (valid) {
        stage = __LINE__;
        const u64 text = scene.text_revision;
        const u32 text_builds = renderer.content_text->rebuilds;
        const u32 paint_builds = renderer.paint_stream_builds;
        const auto play = scene.projection.controls[0].bounds;
        ui_video_scene_pointer(&scene, UiVideoPointerEvent::move,
                               {(play.left + play.right) * .5f, (play.top + play.bottom) * .5f});
        valid = scene.text_revision == text && probe_present(&renderer, kDefaultTheme) &&
                renderer.paint_stream_builds > paint_builds &&
                renderer.content_text->rebuilds == text_builds;
        state.elapsed_ms += 500;
        valid = valid && project(minimum(640.f, available)) && scene.text_revision == text &&
                probe_present(&renderer, kDefaultTheme) && renderer.content_text->rebuilds == text_builds;
        state.elapsed_ms += 500;
        valid = valid && project(minimum(640.f, available)) && scene.text_revision != text &&
                !d3d11_renderer_interaction_text_state_current(renderer);
        const u32 suppressed = renderer.stale_interaction_text_suppressions;
        valid = valid && probe_present(&renderer, kDefaultTheme) &&
                renderer.stale_interaction_text_suppressions > suppressed &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme) &&
                d3d11_renderer_interaction_text_state_current(renderer);
    }
    if (valid) {
        stage = __LINE__;
        const auto original = state;
        const auto prefix = renderer.content_text_prefix;
        valid = semantic_command && prefix.storage == renderer.content_text && prefix.quads != 0 &&
                prefix.semantics != 0 && prefix.hits != 0 && prefix.base_builds != 0;
        const u64 base = prefix_signature(renderer);
        u32 suffix = prefix.overlay_builds;
        const auto suffix_update = [&](bool text_changed = true) noexcept {
            if (!project(minimum(640.f, available)) ||
                !renderer_rebuild_content_text(&renderer, kDefaultTheme))
                return false;
            const auto& current = renderer.content_text_prefix;
            suffix += text_changed ? 1 : 0;
            return current.base_builds == prefix.base_builds && current.overlay_builds == suffix &&
                   prefix_cursors_equal(prefix, current) && prefix_signature(renderer) == base;
        };
        // Crossing a digit/width boundary and growing/retiring the status label
        // must leave the warm shell/message quads, semantics and effects intact.
        state.elapsed_ms = 599000;
        valid = valid && suffix_update();
        const u32 same_second = renderer.content_text->rebuilds;
        state.elapsed_ms = 599999;
        valid = valid && project(minimum(640.f, available)) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                renderer.content_text->rebuilds == same_second;
        state.elapsed_ms = 600000;
        valid = valid && suffix_update();
        state.buffering = false;
        valid = valid && suffix_update(false);
        state.error = true;
        valid = valid && suffix_update();
        state.error = false;
        state.buffering = true;
        valid = valid && suffix_update();
        renderer.video_scene = nullptr;
        valid = valid && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                renderer.content_text_prefix.base_builds == prefix.base_builds &&
                prefix_signature(renderer) == base && renderer.interaction_text_video_revision == 0 &&
                renderer.content_text->content_semantic_count == prefix.semantics &&
                renderer.content_text->content_hit_region_count == prefix.hits &&
                renderer.text_effect_count == prefix.effects &&
                renderer.text_effect_animated == prefix.animated;
        renderer.video_scene = &scene;
        state = original;
        valid = valid && project(minimum(640.f, available)) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme);
        const auto full_rebuild = [&](const Theme& theme = kDefaultTheme) noexcept {
            const u32 builds = renderer.content_text_prefix.base_builds;
            return renderer_rebuild_content_text(&renderer, theme) &&
                   renderer.content_text_prefix.base_builds > builds;
        };
        // A real shared-atlas reset must rebuild even unchanged video text.
        if (valid)
            stage = __LINE__;
        d3d11_glyph_text_reset_atlas(&renderer.glyph_text);
        valid = valid && full_rebuild();
        if (valid)
            stage = __LINE__;
        Theme changed = kDefaultTheme;
        changed.text_primary.red += .03125f;
        valid = valid && full_rebuild(changed) && full_rebuild();
        if (valid)
            stage = __LINE__;
        const u32 dpi = renderer.dpi;
        ++renderer.dpi;
        valid = valid && full_rebuild();
        renderer.dpi = dpi;
        valid = valid && full_rebuild();
        if (valid)
            stage = __LINE__;
        const u32 selected = renderer.message_text_selection_revision;
        ++renderer.message_text_selection_revision;
        valid = valid && full_rebuild();
        renderer.message_text_selection_revision = selected;
        valid = valid && full_rebuild();
        if (renderer.message_text) {
            auto& messages = *const_cast<MessageTextPresentation*>(renderer.message_text);
            const u64 signature = messages.projection_signature;
            if (messages.command_count) {
                const u32 revision = messages.commands[0].message_revision;
                const u64 identity = messages.commands[0].measurement_identity;
                ++messages.commands[0].message_revision;
                // Production rebuilds measurement identity with source revision;
                // the projection signature intentionally consumes that identity.
                messages.commands[0].measurement_identity = hash64_mix(identity, revision + 1);
                messages.projection_signature = message_text_presentation_projection_signature(messages);
                if (valid)
                    stage = __LINE__;
                valid = valid && full_rebuild();
                messages.commands[0].message_revision = revision;
                messages.commands[0].measurement_identity = identity;
                messages.projection_signature = signature;
                valid = valid && full_rebuild();
            } else
                valid = false;
        } else
            valid = false;
        // A recycled cached root cannot qualify an overlay-only update. The
        // actual emitter continues using and resolving the current real root.
        renderer.content_text_prefix.message_root.handle.generation ^= 1;
        state.elapsed_ms += 1000;
        if (valid)
            stage = __LINE__;
        valid = valid && project(minimum(640.f, available)) && full_rebuild() &&
                probe_present(&renderer, kDefaultTheme);
        auto& real_messages = *const_cast<MessageScene*>(renderer.message_scene);
        const auto root = real_messages.root;
        real_messages.root.generation ^= 1;
        const bool stale_root_refused = !renderer_rebuild_content_text(&renderer, kDefaultTheme);
        real_messages.root = root;
        const auto retired = renderer.content_text_prefix;
        if (valid)
            stage = __LINE__;
        valid = valid && stale_root_refused && renderer.content_text->quad_count == retired.quads &&
                renderer.content_text->content_semantic_count == retired.semantics &&
                renderer.content_text->content_hit_region_count == retired.hits &&
                renderer.text_effect_count == retired.effects &&
                renderer.text_effect_animated == retired.animated && full_rebuild() &&
                probe_present(&renderer, kDefaultTheme);
        log_u64(LogLevel::info, "probe", "video_text_prefix_metadata_bytes", sizeof(RendererTextPrefix));
        log_u64(LogLevel::info, "probe", "video_text_base_builds",
                renderer.content_text_prefix.base_builds);
        log_u64(LogLevel::info, "probe", "video_text_overlay_builds",
                renderer.content_text_prefix.overlay_builds);
    }
    if (valid) {
        stage = __LINE__;
        valid = project(256) && scene.projection.compact;
        const auto seek = scene.projection.controls[1].bounds;
        const auto down =
            ui_video_scene_pointer(&scene, UiVideoPointerEvent::down,
                                   {(seek.left + seek.right) * .5f, (seek.top + seek.bottom) * .5f});
        const auto up =
            ui_video_scene_pointer(&scene, UiVideoPointerEvent::up, {seek.right + 500, seek.top});
        valid = valid && down.request_capture && down.action_requested && up.release_capture &&
                up.action_requested && up.action.position_ms == state.duration_ms &&
                scene.projection.playback.elapsed_ms == state.elapsed_ms &&
                ui_video_action_current(state, up.action) &&
                ui_video_scene_focus(&scene, UiVideoControl::seek, true) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme) &&
                capture_native_video_preview(renderer, scene, capture);
        const u64 rev = scene.text_revision;
        state.error = true;
        state.buffering = false;
        ++state.revision;
        valid = valid && project(256) && scene.text_revision != rev &&
                scene.focus == UiVideoControl::none &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme);
    }
    if (valid) {
        stage = __LINE__;
        UiVideoPlayAffordanceStyle affordance{};
        state.error = false;
        ++state.revision;
        const UiRect previews[]{{left, top, left + 80, top + 60},
                                {left, top, left + minimum(640.f, available), top + 48}};
        for (const auto& preview : previews) {
            if (valid)
                stage = __LINE__;
            valid = valid &&
                    ui_video_scene_project_play_only(&scene, state, preview, viewport, controls, style,
                                                     affordance, kDefaultTheme) &&
                    scene.play_only && ui_video_scene_covers_preview(scene, preview);
            if (valid)
                stage = __LINE__;
            valid = valid && ui_video_scene_focus(&scene, UiVideoControl::play_pause, true) &&
                    renderer_rebuild_content_text(&renderer, kDefaultTheme);
            if (valid)
                stage = __LINE__;
            valid = valid && probe_present(&renderer, kDefaultTheme) &&
                    d3d11_renderer_interaction_text_state_current(renderer);
        }
        if (valid)
            stage = __LINE__;
        // Exercise the same poster painter used by the native media overlay
        // against a cropped preview; GPU captures remain bounded to one frame.
        PaintStream poster{};
        const UiRect preview = previews[0], clip{left + 40, top, left + 80, top + 60};
        valid = valid && paint_stream_initialize(&poster) &&
                paint_stream_reset(&poster, static_cast<float>(renderer.width),
                                   static_cast<float>(renderer.height)) &&
                ui_video_play_affordance_paint(preview, clip, affordance, kDefaultTheme, &poster) &&
                poster.count > 1;
        for (u32 i = 0; valid && i < poster.batch_count; ++i) {
            const auto& pc = poster.clips[poster.batches[i].clip_index];
            valid = pc.left == clip.left && pc.right == clip.right && pc.top == clip.top &&
                    pc.bottom == clip.bottom;
        }
        paint_stream_release(&poster);
    }
    if (valid) {
        stage = __LINE__;
        state.error = state.buffering = false;
        state.collection_count = 11; state.collection_track = 4; state.can_change_track = true;
        ++state.revision;
        const float widths[]{196, 360};
        const wchar_t* names[]{L"video-scene-collection-narrow.bmp", L"video-scene-collection-wide.bmp"};
        for (u32 i=0; valid && i<2; ++i) {
            valid = project(widths[i]);
            const auto track_bounds = scene.projection.controls[9].bounds;
            ui_video_scene_hover(&scene, {(track_bounds.left + track_bounds.right) * .5f, (track_bounds.top + track_bounds.bottom) * .5f});
            ui_video_scene_titles(&scene, utf8_literal("Current track - Café by Artist"), scene.hover_track, utf8_literal("Hovered song by Another Artist"));
            valid = valid && scene.projection.controls[7].enabled && scene.projection.controls[9].enabled &&
                ui_video_scene_focus(&scene,UiVideoControl::track,true) &&
                renderer_rebuild_content_text(&renderer,kDefaultTheme) && probe_present(&renderer,kDefaultTheme) &&
                (!capture || capture(names[i]));
        }
    }
    if (semantic_command) {
        semantic_command->semantics = original_semantics;
        message_text->projection_signature = original_signature;
    }
    renderer.video_scene = old;
    const bool cleaned =
        renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    if (!valid || !cleaned)
        log_u64(LogLevel::error, "probe", "video_scene_injected_ui_failure_line", stage);
    log_write(valid && cleaned ? LogLevel::info : LogLevel::error, "probe",
              valid && cleaned ? "video_scene_injected_full_play_only_poster_clipping_and_retention_passed"
                               : "video_scene_injected_ui_failed");
    return valid && cleaned;
}
} // namespace maicord
