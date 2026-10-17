#pragma once
#include "probe_present.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
struct ComposerWorkflowProbeState {
    Utf8View reply;
    bool mention = true, mention_visible = true, retained = false;
    Utf8View status;
    bool error = false;
};
using ComposerWorkflowProbePublish = bool (*)(const ComposerWorkflowProbeState&) noexcept;

inline bool composer_workflow_ui_probe(D3D11Renderer& renderer, MessageComposer& model,
    MessageComposerScene& scene, ComposerWorkflowProbePublish publish,
    bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!publish || !renderer.content_text || !renderer.dpi) return false;
    const u32 original_width = renderer.width, original_height = renderer.height;
    message_composer_reset(&model, model.channel_id, false);
    if (message_composer_insert_utf8(&model, utf8_literal("A reply keeps the original author's context.")) !=
        MessageComposerResult::changed) return false;
    ComposerWorkflowProbeState state{};
    state.reply = utf8_literal("Replying to Avery with a deliberately long original author name for native clipping");
    const auto show = [&](const wchar_t* name) noexcept {
        return publish(state) && probe_present(&renderer, kDefaultTheme) && (!capture || capture(name));
    };
    if (!show(L"composer-workflow-reply-mention-on.bmp")) return false;
    const auto* toggle = message_composer_scene_layout_node(scene, MessageComposerControl::reply_mention);
    if (!toggle || !ui_rect_has_area(toggle->rectangle) ||
        message_composer_scene_hit_test(scene, (toggle->rectangle.left + toggle->rectangle.right) * .5f,
            (toggle->rectangle.top + toggle->rectangle.bottom) * .5f) != MessageComposerControl::reply_mention ||
        !message_composer_scene_focus_control(&scene, MessageComposerControl::reply_mention)) return false;
    renderer.focus_owner = &scene.widgets;
    state.mention = false;
    if (!show(L"composer-workflow-reply-mention-off.bmp")) return false;
    message_composer_reset(&model, model.channel_id, false);
    state.reply = utf8_literal("Editing message — empty caption retains the original attachment");
    state.mention_visible = false;
    state.retained = true;
    if (!show(L"composer-workflow-empty-caption-save.bmp") ||
        !message_composer_scene_can_send(scene) ||
        !ui_widget_manager_input_enabled(scene.widgets, scene.send) ||
        message_composer_scene_attachment_label(scene).bytes ||
        ui_widget_manager_layout_node(scene.widgets, scene.attachments)->visible ||
        message_composer_scene_reply_mention_label(scene).bytes) return false;
    state.status = utf8_literal("Could not save this caption. Try again.");
    state.error = true;
    if (!show(L"composer-workflow-caption-error.bmp") || !scene.status_is_error) return false;
    message_composer_reset(&model, model.channel_id, false);
    message_composer_insert_utf8(&model, utf8_literal("Every composer action remains reachable."));
    state = {};
    state.reply = utf8_literal("Replying to Avery with a deliberately long original author name for native clipping");
    const auto metrics = kGuildChannelShellLayoutMetrics;
    const u32 compact_width = static_cast<u32>((metrics.guild_rail_width + metrics.navigation_width +
        kUiShellMinimumMessageWidthDips) * renderer.dpi / 96.f + .5f);
    if (!probe_resize_surface(compact_width, original_height) || !show(L"composer-workflow-narrow.bmp") ||
        ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::member_sidebar)) ||
        !ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::member_toggle)))
        return false;
    // At this width the GIF and sticker buttons fold into the emoji picker.
    const MessageComposerControl controls[]{MessageComposerControl::add_attachment,
        MessageComposerControl::edit, MessageComposerControl::emoji, MessageComposerControl::send};
    for (const auto control : controls) {
        const auto* node = message_composer_scene_layout_node(scene, control);
        if (!node || !ui_rect_has_area(node->rectangle) ||
            !ui_rect_equal(node->rectangle, ui_rect_intersection(node->rectangle, node->clip_rectangle)) ||
            message_composer_scene_hit_test(scene, (node->rectangle.left + node->rectangle.right) * .5f,
                (node->rectangle.top + node->rectangle.bottom) * .5f) != control) return false;
    }
    const u32 layout = scene.layout_passes, shell_layout = renderer.ui_scene->layout_passes,
        builds = renderer.content_text->rebuilds, raster = renderer.glyph_text.cache_builds,
        paint_uploads = renderer.paint_instance_uploads, text_uploads = renderer.text_instance_uploads,
        media_uploads = renderer.media_instance_uploads, painted = renderer.painted_frames,
        skips = renderer.unchanged_present_skips;
    for (u32 i = 0; i < 64; ++i)
        if (!publish(state) || !probe_present(&renderer, kDefaultTheme)) return false;
    if (scene.layout_passes != layout || renderer.ui_scene->layout_passes != shell_layout ||
        renderer.content_text->rebuilds != builds || renderer.glyph_text.cache_builds != raster ||
        renderer.paint_instance_uploads != paint_uploads || renderer.text_instance_uploads != text_uploads ||
        renderer.media_instance_uploads != media_uploads || renderer.painted_frames != painted ||
        renderer.unchanged_present_skips - skips != 64) return false;
    log_write(LogLevel::info, "probe", "composer_workflow_64_zero_layout_shape_raster_upload_draw_passed");
    if (!probe_resize_surface(original_width, original_height) || !publish(state) ||
        !ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::member_sidebar)))
        return false;
    log_write(LogLevel::info, "probe", "composer_workflow_reply_caption_error_responsive_passed");
    return true;
}
} // namespace maicord
