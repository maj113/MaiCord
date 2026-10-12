#pragma once
#include "probe_present.hpp"
#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
bool probe_resize_surface(u32 width, u32 height) noexcept;
namespace message_forward_probe_detail {
struct Source { u32 calls{}; bool current{true}; };
inline bool current(void* opaque) noexcept { return static_cast<Source*>(opaque)->current; }
inline bool resolve(void* opaque, u32 index, UiMessageForwardRow* row) noexcept {
    ++static_cast<Source*>(opaque)->calls;
    const Utf8View labels[]{utf8_literal("release-planning"), utf8_literal("Avery Chen"),
        utf8_literal("design-studio"), utf8_literal("Native UI study group"), utf8_literal("project-notes")};
    const Utf8View details[]{utf8_literal("MaiCord Lab • Text channel"), utf8_literal("Direct conversation"),
        utf8_literal("MaiCord Lab • Text channel"), utf8_literal("Group conversation • 4 participants"),
        utf8_literal("Workshop • Text channel")};
    const UiQuickSwitcherKind kinds[]{UiQuickSwitcherKind::text_channel, UiQuickSwitcherKind::private_channel,
        UiQuickSwitcherKind::text_channel, UiQuickSwitcherKind::group_private, UiQuickSwitcherKind::text_channel};
    const auto i = index % 5;
    *row = {1000 + index, 71, 1000 + index, 0, kinds[i], labels[i], details[i]};
    return true;
}
}
inline bool message_forward_ui_probe(D3D11Renderer& renderer,
    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    if (!renderer.content_text || !renderer.dpi) return false;
    UiMessageForwardScene scene{};
    message_forward_probe_detail::Source fixture{};
    UiMessageForwardSource source{&fixture, 5, 1, message_forward_probe_detail::resolve, message_forward_probe_detail::current};
    UiMessageForwardContent content{};
    content.author = utf8_literal("Avery Chen");
    content.preview = utf8_literal("The prototype is ready for review. Share this snapshot with the team so we can compare notes.");
    content.media_summary = utf8_literal("1 image • interface-study.png");
    content.edit = {{}, 0, 0, true, true, true};
    content.caption = {{}, 0, 0, true, false, false};
    content.revision = 1;
    const u32 width = renderer.width, height = renderer.height, dpi = renderer.dpi;
    struct Restore {
        D3D11Renderer& renderer; UiMessageForwardScene& scene;
        const UiMessageForwardScene* old_scene; const UiWidgetManager* focus;
        const MessageActionBar* old_bar;
        ~Restore() noexcept {
            renderer.message_forward_scene = old_scene; renderer.focus_owner = focus;
            renderer.message_action_bar = old_bar;
            renderer.retained_presentation.valid = false;
            content_text_retention_invalidate(&renderer.content_text_retention);
            ui_message_forward_scene_release(&scene);
        }
    } restore{renderer, scene, renderer.message_forward_scene, renderer.focus_owner, renderer.message_action_bar};
    Theme theme = kDefaultTheme;
    const auto show = [&](const wchar_t* name) noexcept {
        const UiRect v{0,0,renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi};
        if (!ui_message_forward_scene_update(&scene, &source, &content, v)) return false;
        renderer.message_forward_scene = &scene; renderer.focus_owner = &scene.widgets;
        return renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme) &&
            (!content.committed || !content.caption.text.bytes ||
                (renderer.popup_edit_layout.active &&
                 renderer.popup_edit_layout.text_bytes == content.caption.text.bytes &&
                 renderer.popup_edit_layout.text_signature == text_edit_signature(content.caption.text) &&
                 !renderer.popup_edit_layout.anchor && !renderer.popup_edit_layout.caret_offset &&
                 renderer.popup_edit_layout.scroll_x == 0 &&
                 renderer.popup_edit_layout.width > 0 && renderer.popup_edit_layout.layout_height > 0)) &&
            (!capture || capture(name, theme));
    };
    if (!show(L"message-forward-ready.bmp")) return false;
    const auto edit = scene.edit, caption = scene.caption;
    const auto unchanged = [&]() noexcept {
        const u32 calls = fixture.calls, layout = scene.widgets.layout.revision,
            builds = renderer.content_text->rebuilds, raster = renderer.glyph_text.cache_builds,
            paint_uploads = renderer.paint_instance_uploads, text_uploads = renderer.text_instance_uploads,
            media_uploads = renderer.media_instance_uploads, painted = renderer.painted_frames,
            skips = renderer.unchanged_present_skips;
        for (u32 i = 0; i < 64; ++i)
            if (!ui_message_forward_scene_update(&scene, &source, &content, scene.viewport) ||
                !renderer_rebuild_content_text(&renderer, theme) || !probe_present(&renderer, theme)) return false;
        return fixture.calls == calls && scene.widgets.layout.revision == layout && renderer.content_text->rebuilds == builds &&
            renderer.glyph_text.cache_builds == raster && renderer.paint_instance_uploads == paint_uploads &&
            renderer.text_instance_uploads == text_uploads && renderer.media_instance_uploads == media_uploads &&
            renderer.painted_frames == painted && renderer.unchanged_present_skips - skips == 64;
    };
    if (!unchanged()) return false;
    log_write(LogLevel::info, "probe", "message_forward_64_zero_resolve_layout_shape_raster_upload_draw_passed");
    content.selected_count = 5; content.can_send = true;
    for (u32 i = 0; i < 5; ++i) {
        UiMessageForwardRow row{};
        if (!source.resolve(source.context, i, &row)) return false;
        content.selected[i] = {row.channel_id, row.label, {}, UiMessageForwardState::ready};
    }
    ++content.revision;
    if (!show(L"message-forward-five-targets.bmp")) return false;
    content.caption = {utf8_literal("For our next review — let’s compare notes."), 0, 0, true, true, true};
    content.caption.anchor = content.caption.caret = content.caption.text.bytes;
    content.edit.focused = false; ++content.revision;
    if (!show(L"message-forward-caption.bmp")) return false;
    content.sending = true; content.can_send = false; content.committed = true;
    content.edit.active = content.caption.active = false;
    for (auto& target : content.selected) target.state = UiMessageForwardState::sending;
    ++content.revision;
    if (!show(L"message-forward-sending.bmp")) return false;
    if (!unchanged()) return false;
    log_write(LogLevel::info, "probe", "message_forward_committed_caption_64_zero_resolve_layout_shape_raster_upload_draw_passed");
    content.sending = false;
    for (auto& target : content.selected) target.state = UiMessageForwardState::failed;
    content.status = utf8_literal("Could not send. Select a failed destination to retry."); ++content.revision;
    if (!show(L"message-forward-failed.bmp")) return false;
    content.selected[0].state = content.selected[1].state = content.selected[4].state = UiMessageForwardState::sent;
    content.status = utf8_literal("3 sent • 2 need retry"); ++content.revision;
    if (!show(L"message-forward-partial-success.bmp") ||
        ui_message_forward_scene_target(scene, scene.targets[0]).action != UiMessageForwardAction::none ||
        ui_message_forward_scene_target(scene, scene.targets[2]).action != UiMessageForwardAction::retry) return false;
    for (auto& target : content.selected) target.state = UiMessageForwardState::sent;
    content.status = utf8_literal("Message forwarded."); ++content.revision;
    if (!show(L"message-forward-completed.bmp") ||
        ui_message_forward_scene_target(scene, scene.send).action != UiMessageForwardAction::close) return false;
    content.selected[2].state = content.selected[3].state = UiMessageForwardState::failed;
    content.status = utf8_literal("3 sent • 2 need retry"); ++content.revision;
    source.count = 100; ++source.revision; ++content.revision;
    if (!show(L"message-forward-many-results.bmp") || !ui_message_forward_scene_move(&scene, UiQuickSwitcherMove::last) ||
        scene.selected != 99 || !show(L"message-forward-keyboard-last.bmp")) return false;
    source.count = 0; content.phase = UiMessageForwardPhase::no_matches;
    content.edit = {utf8_literal("unmatched destination"), 21, 21, true, true, true};
    content.caption.focused = false; ++source.revision; ++content.revision;
    if (!show(L"message-forward-empty.bmp")) return false;
    source.count = 5; content.phase = UiMessageForwardPhase::ready;
    content.edit = {{},0,0,true,true,true}; ++source.revision; ++content.revision;
    if (!probe_resize_surface(static_cast<u32>(360.f * renderer.dpi / 96.f + .5f),
        static_cast<u32>(540.f * renderer.dpi / 96.f + .5f)) || !show(L"message-forward-narrow.bmp") ||
        scene.edit.slot != edit.slot || scene.edit.generation != edit.generation ||
        scene.caption.slot != caption.slot || scene.caption.generation != caption.generation) return false;
    if (!probe_resize_surface(static_cast<u32>(360.f * renderer.dpi / 96.f + .5f),
        static_cast<u32>(420.f * renderer.dpi / 96.f + .5f)) || !show(L"message-forward-short-receipts.bmp") ||
        !scene.receipts_visible || ui_rect_height(scene.body) < 34 ||
        ui_message_forward_scene_target(scene, scene.targets[2]).action != UiMessageForwardAction::retry) return false;
    renderer_set_dpi(&renderer, 144);
    if (!probe_resize_surface(1440, 1080) || !show(L"message-forward-dpi144.bmp")) return false;
    theme = kLightTheme;
    if (!show(L"message-forward-light.bmp")) return false;
    theme = kDefaultTheme; theme.accent = {.95f, .85f, .4f, 1};
    theme = ui_theme_resolve_defaults(theme); // semantic tokens follow the core accent
    content.committed = content.sending = false; content.can_send = true;
    content.edit.active = content.caption.active = true;
    for (auto& target : content.selected) target.state = UiMessageForwardState::ready;
    content.status = {}; ++content.revision;
    if (!show(L"message-forward-bright-accent.bmp")) return false;
    const auto foreground = ui_surface_foreground(theme, theme.accent);
    const auto equal_color = [](Color a, Color b) noexcept {
        return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
    };
    UiTextCommand commands[64]{}; UiTextCommandStream stream{}; UiTextOutput output{&stream, nullptr};
    if (!ui_text_command_stream_initialize(&stream, {commands, 64}) ||
        !ui_message_forward_scene_text_output(scene, theme, &output)) return false;
    bool label_color = false;
    for (u32 i = 0; i < stream.count; ++i)
        if (commands[i].owner.manager == &scene.widgets && commands[i].owner.handle.slot == scene.send.slot)
            label_color = equal_color(commands[i].color, foreground);
    PaintStream paint{};
    bool icon_color = false;
    bool contrast = label_color && paint_stream_initialize(&paint) &&
        paint_stream_reset(&paint, scene.viewport.right, scene.viewport.bottom) &&
        ui_message_forward_scene_paint(scene, theme, &paint, nullptr, nullptr, renderer.dpi);
    for (u32 i = 0; contrast && i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        if (equal_color(q.fill, foreground) && ui_rect_has_area(ui_rect_intersection(
            {q.left, q.top, q.right, q.bottom}, scene.send_rectangle))) icon_color = true;
    }
    paint_stream_release(&paint);
    if (!contrast || !icon_color || ui_background_luminance(foreground) >= .2f) return false;
    theme = kLightTheme; theme.accent = {.95f, .85f, .4f, 1};
    theme = ui_theme_resolve_defaults(theme);
    if (!show(L"message-forward-light-bright-accent.bmp")) return false;
    fixture.current = false;
    if (ui_message_forward_scene_selected(scene).action != UiMessageForwardAction::none) return false;
    fixture.current = true;
    renderer_set_dpi(&renderer, dpi);
    if (!probe_resize_surface(width, height)) return false;
    MessageActionBar launcher{};
    const auto* messages = renderer.message_scene;
    const MessageSceneRowWidget* source_row = nullptr;
    if (messages && messages->active && messages->timeline && messages->timeline->store)
        for (u32 index = 0; index < messages->rows.count; ++index) {
            const auto& row = messages->rows[index];
            if (row.message_id) { source_row = &row; break; }
        }
    const auto* source_message = source_row ? message_store_resolve(*messages->timeline->store, source_row->message) : nullptr;
    if (!source_message || !message_action_bar_open(&launcher, *messages,
        {source_row->message_id, source_row->widget, source_row->source_index}, *source_message, nullptr, true).ok() ||
        launcher.item_count != kMessageActionBarMaximumItems) return false;
    renderer.message_forward_scene = nullptr;
    renderer.message_action_bar = &launcher;
    renderer.focus_owner = &messages->widgets;
    if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) || !probe_present(&renderer, kDefaultTheme) ||
        (capture && !capture(L"message-forward-hover-launcher.bmp", kDefaultTheme))) return false;
    renderer.message_action_bar = restore.old_bar;
    log_write(LogLevel::info, "probe", "message_forward_five_caption_fail_retry_partial_minimum_themes_dpi_passed");
    return true;
}
} // namespace maicord
