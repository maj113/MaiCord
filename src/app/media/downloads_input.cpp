#include "downloads_internal.hpp"
#include "maicord/app/shell/shortcut_registry.hpp"
#include "maicord/ui/core/scene_cursor.hpp"
#include "../shell/content_input.hpp"
#include "../shell/input.hpp"
#include "../shell/window_events.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord::app_detail {

bool application_downloads_pump() noexcept {
    auto* q = g_app.downloads;
    if (!q)
        return true;
    if (!account_current(*q) && !q->closing)
        application_downloads_prepare_account_stop();
    if (!q->closing && q->picker_job != kNoDownload) {
        auto& choosing = q->jobs[q->picker_job];
        if (choosing.phase == JobPhase::choosing && !choosing.cancel_requested &&
            (!source_current(choosing.source) || !source_media_allowed(choosing.source))) {
            choosing.cancel_requested = true;
            fail_job(*q, choosing, HttpsFileDownloadError::invalid_input);
            native_file_picker_cancel(&q->picker);
        }
    }
    picker_complete(*q);
    if (q->active_job != kNoDownload) {
        auto& j = q->jobs[q->active_job];
        if (q->account_instance == g_app.account.instance && g_app.account.api.download) {
            const auto state = g_app.account.api.download(q->account_instance,
                                                          AccountDownloadOperation::status, j.id, nullptr);
            if (state.id == j.id) {
                if (!bytes_equal(&j.transfer, &state.transfer, sizeof(j.transfer))) {
                    j.transfer = state.transfer;
                    changed(*q);
                }
                if (state.transfer.phase == HttpsFileDownloadPhase::complete) {
                    const auto released = g_app.account.api.download(
                        q->account_instance, AccountDownloadOperation::release, j.id, nullptr);
                    if (released.released) {
                        j.admitted = false;
                        q->active_job = kNoDownload;
                        j.phase = state.transfer.ok() ? JobPhase::saved
                                  : state.transfer.error == HttpsFileDownloadError::cancelled
                                      ? JobPhase::cancelled
                                      : JobPhase::failed;
                        if (j.phase == JobPhase::failed) {
                            log_failure(state.transfer);
                            q->reveal = true;
                        }
                        if (j.phase != JobPhase::cancelled)
                            toast_result(j);
                        changed(*q);
                    }
                }
            }
        } else {
            j.admitted = false;
            j.phase = JobPhase::cancelled;
            q->active_job = kNoDownload;
            changed(*q);
        }
    }
    if (q->closing) {
        const bool close = q->close_window;
        application_downloads_close();
        if (application_downloads_release() && close && g_app.window)
            window_request_close(g_app.window);
        return true;
    }
    // The save dialog and a transfer run side by side, one of each at a time, so
    // a save chosen during another download asks for its location at once.
    bool resolution_pending = false;
    for (u32 i = 0; i < kDownloadJobs; ++i) {
        auto& j = q->jobs[i];
        if (!j.occupied || terminal(j.phase) || i == q->picker_job || i == q->active_job)
            continue;
        if (!source_current(j.source) || !source_media_allowed(j.source)) {
            fail_job(*q, j, HttpsFileDownloadError::invalid_input);
            continue;
        }
        if (j.phase == JobPhase::warning) {
            if (!q->warning_id) {
                q->warning_id = j.id;
                changed(*q);
            }
            continue;
        }
        if (j.phase == JobPhase::queued) {
            if (q->picker_job != kNoDownload)
                continue;
            const auto error = native_file_picker_save_start(&q->picker, g_app.window, {j.source.filename, j.source.filename_bytes},
            utf8_literal("Save attachment"));
            if (error != NativeFilePickerError::none) {
                fail_job(*q, j, HttpsFileDownloadError::file_open);
                continue;
            }
            q->picker_job = i;
            j.phase = JobPhase::choosing;
            changed(*q);
            continue;
        }
        if (j.phase != JobPhase::resolving || !j.approved || !j.path || q->active_job != kNoDownload ||
            resolution_pending)
            continue;
        const ReceivedAttachmentOpenIdentity identity{
            j.source.scope,      j.source.channel,  j.source.message,
            j.source.attachment, j.source.revision, j.source.snapshot ? j.source.origin_channel : 0};
        Utf8View ready{};
        const Utf8View original{j.source.url, j.source.url_bytes};
        // Embed media is fetched by the media-proxy URL it was captured with;
        // only signed attachment links are refreshed first.
        auto result = ReceivedAttachmentOpenResult::ready;
        if (j.source.embed)
            ready = original;
        else
            result = j.resolution.pending
                         ? received_attachment_open_resume(
                               &j.resolution, identity, original, &g_app.media_urls,
                               clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, &ready)
                         : received_attachment_open_begin(&j.resolution, identity, original,
                                                          &g_app.media_urls, clock_monotonic_milliseconds(),
                                                          clock_unix_milliseconds() / 1000, &ready,
                                                          j.force_refresh);
        if (result == ReceivedAttachmentOpenResult::pending) {
            resolution_pending = true;
            continue;
        }
        if (result != ReceivedAttachmentOpenResult::ready || !source_current(j.source) ||
            !source_media_allowed(j.source) ||
            (!j.source.embed && !received_attachment_url_current(ready, j.source.origin_channel, j.source.attachment))) {
            fail_job(*q, j,
                     result == ReceivedAttachmentOpenResult::stale ? HttpsFileDownloadError::invalid_input
                                                                   : HttpsFileDownloadError::http);
            continue;
        }
        AccountDownloadRequest request{};
        request.url = ready;
        request.path = {j.path, j.path_units};
        request.source_channel_id = j.source.embed ? 0 : j.source.origin_channel;
        request.attachment_id = j.source.attachment;
        request.expected_bytes = j.source.bytes;
        request.destination = j.destination;
        request.destination_exists = j.destination_exists;
        request.overwrite_authorized = j.overwrite_authorized;
        const auto started = g_app.account.api.download(g_app.account.instance,
                                                        AccountDownloadOperation::start, j.id, &request);
        if (started.submit_error != HttpsFileDownloadError::none || started.id != j.id) {
            if (!started.released && started.id == j.id) {
                j.admitted = true;
                j.phase = JobPhase::transferring;
                j.transfer = started.transfer;
                j.transfer.error = started.submit_error;
                q->active_job = i;
                changed(*q);
                continue;
            }
            fail_job(*q, j,
                     started.submit_error == HttpsFileDownloadError::none ? HttpsFileDownloadError::worker
                                                                          : started.submit_error);
            continue;
        }
        j.force_refresh = false;
        j.admitted = true;
        j.phase = JobPhase::transferring;
        j.transfer = started.transfer;
        q->active_job = i;
        changed(*q);
    }
    if (q->reveal) {
        q->reveal = false;
        if (!q->active)
            application_downloads_open();
    }
    q->wake_at = q->picker_job != kNoDownload || (q->active && q->active_job != kNoDownload)
                     ? deadline_after(clock_monotonic_milliseconds(), 100)
                     : 0;
    return application_downloads_synchronize();
}
bool application_downloads_save_target(float x, float y) noexcept {
    DownloadSource source;
    return attachment_at(x, y, true, &source);
}
bool application_downloads_save_press(float x, float y) noexcept {
    DownloadSource source;
    UiRect bounds{};
    UiWidgetHandle widget{};
    if (!attachment_at(x, y, true, &source, {}, &bounds, &widget))
        return false;
    auto* q = ensure_owner();
    if (!q)
        return true;
    q->context = source;
    q->inline_capture = true;
    q->pressed_bounds = bounds;
    q->pressed_widget = widget;
    ui_widget_manager_capture(&g_app.message_scene.widgets, widget);
    ui_widget_manager_focus(&g_app.message_scene.widgets, widget);
    application_request_frame();
    if (g_app.window)
        window_capture_pointer(g_app.window);
    return true;
}
bool application_downloads_save_focused() noexcept {
    DownloadSource source;
    return g_app.message_scene.widgets.focused.generation &&
           attachment_at(0, 0, false, &source, g_app.message_scene.widgets.focused) && begin_source(source);
}
bool application_downloads_context_at(float x, float y) noexcept {
    DownloadSource source;
    const UiRect anchor{x, y, x + 1, y + 1};
    return attachment_at(x, y, false, &source) ? open_context(source, anchor)
           : sticker_at(x, y, {}, &source)     ? open_sticker_context(source, anchor)
                                               : embed_context(x, y, anchor);
}
bool application_downloads_context_focused() noexcept {
    DownloadSource source;
    const auto focused = g_app.message_scene.widgets.focused;
    return attachment_at(0, 0, false, &source, focused) ? open_context(source, g_app.message_scene.viewport)
           : sticker_at(0, 0, focused, &source) ? open_sticker_context(source, g_app.message_scene.viewport)
                                                : false;
}
bool application_downloads_context_action(u32 action) noexcept {
    auto* q = g_app.downloads;
    // Only the message menu this owner opened, still showing, carries the rows.
    if (!received_attachment_menu_action(action) || !q || !q->menu_valid ||
        g_app.popup_owner_kind != PopupOwnerKind::message || !g_app.popup_scene.active ||
        !g_app.message_context.active || g_app.message_context.opens != q->menu_opens ||
        g_app.message_context.message_id != q->menu.message)
        return false;
    const auto source = q->menu;
    q->menu_valid = false;
    if (!dismiss_popup())
        return false;
    const Utf8View url{source.url, source.url_bytes};
    if (q->menu_kind == ReceivedAttachmentKind::sticker) {
        const auto found = message_store_find(g_app.message_view.store, source.message);
        const auto* message =
            found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        if (!message || message->revision != source.revision)
            return true;
        if (action == kReceivedAttachmentActionCopyLink)
            copy_link(url);
        if (action == kReceivedAttachmentActionOpenLink)
            application_open_content_url(url);
        return true;
    }
    if (!source_current(source))
        return true;
    if (source.embed) {
        const Utf8View link{q->menu_link, q->menu_link_bytes};
        if (action == kReceivedAttachmentActionSave)
            return begin_source(source);
        if (action == kReceivedAttachmentActionCopyLink)
            copy_link(link);
        if (action == kReceivedAttachmentActionOpenLink)
            application_open_content_url(link);
        return true;
    }
    switch (action) {
    case kReceivedAttachmentActionSave:
        return begin_source(source);
    case kReceivedAttachmentActionCopyImage:
        if (q->menu_kind == ReceivedAttachmentKind::image)
            copy_image(source, q->menu_format);
        return true;
    case kReceivedAttachmentActionCopyLink:
        copy_link(url);
        return true;
    case kReceivedAttachmentActionOpenLink:
        application_received_attachment_open(source.message, source.attachment,
                                             source.snapshot ? source.origin_channel : 0);
        return true;
    default:
        return true;
    }
}
bool application_downloads_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    if (kind == UiInputKind::character &&
        ((g_app.downloads_held_key == 'J' && key == 10) || (g_app.downloads_held_key == 'S' && key == 19)))
        return true;
    if (ui_input_consume_key_tail(&g_app.downloads_held_key, {kind, key, repeated}))
        return true;
    if (kind == UiInputKind::key_down && shortcut_matches(ShortcutAction::downloads, shortcut_chord_from_keyboard(key))) {
        g_app.downloads_held_key = key;
        if (!repeated)
            application_downloads_open();
        return true;
    }
    if (!application_downloads_active() && kind == UiInputKind::key_down &&
        g_app.context_focus == &g_app.message_scene.widgets &&
        shortcut_matches(ShortcutAction::save_attachment, shortcut_chord_from_keyboard(key)) &&
        application_downloads_save_focused()) {
        g_app.downloads_held_key = key;
        return true;
    }
    auto* q = g_app.downloads;
    if (!q || !q->active)
        return false;
    if (!scene_current(q)) {
        const bool yield = foreground_form();
        application_downloads_close();
        return !yield;
    }
    if (kind == UiInputKind::key_down) {
        if (key == key::escape) {
            g_app.downloads_held_key = key;
            application_downloads_close();
        } else if (key == key::tab) {
            g_app.downloads_held_key = key;
            ui_widget_manager_focus(&q->scene.widgets, ui_widget_manager_next_focus(
                                                           q->scene.widgets, q->scene.widgets.focused,
                                                           application_shift_down(), true));
            application_request_frame();
        } else if ((key == key::enter || key == key::space) && !repeated) {
            g_app.downloads_held_key = key;
            activate(*q, ui_download_scene_target(q->scene, q->scene.widgets.focused));
        } else if (key == key::page_up || key == key::page_down)
            activate(*q, {key == key::page_up ? UiDownloadAction::previous : UiDownloadAction::next});
    }
    return true;
}
bool application_downloads_filter(const WindowEvent& event) noexcept {
    return input_plain_key(event) &&
           ui_input_consume_key_tail(
               &g_app.downloads_held_key,
               {event.type == EventType::key_down ? UiInputKind::key_down : UiInputKind::key_up, event.key.key,
                input_key_was_down(event)});
}

namespace {

// The pointer events that end or cancel an inline attachment press. Out of line: its
// DownloadSource temporary would otherwise give every window message a multi-KB frame.
__declspec(noinline) bool inline_press_event(ApplicationDownloads& q, const WindowEvent& event) noexcept {
    if (input_release(event)) {
        const auto& pressed = q.context; // only a save press rewrites it
        const auto widget = q.pressed_widget;
        const auto bounds = q.pressed_bounds;
        const bool owned = ui_widget_handle_matches(g_app.message_scene.widgets.captured, widget);
        q.inline_capture = false;
        if (owned)
            ui_widget_manager_capture(&g_app.message_scene.widgets, ui_widget_invalid_handle());
        application_release_capture();
        DownloadSource live;
        UiRect current_bounds{};
        UiWidgetHandle current_widget{};
        if (owned && source_current(pressed) &&
            attachment_at(client_dip_x(event), client_dip_y(event), true, &live, {}, &current_bounds,
                          &current_widget) &&
            ui_widget_handle_equal(current_widget, widget) &&
            bytes_equal(&bounds, &current_bounds, sizeof(bounds)) && live.message == pressed.message &&
            live.attachment == pressed.attachment && live.snapshot == pressed.snapshot &&
            live.revision == pressed.revision && live.handle.generation == pressed.handle.generation &&
            live.fingerprint == pressed.fingerprint)
            begin_source(pressed);
        return true;
    }
    if (event.type == EventType::capture_lost || event.type == EventType::cancel_input || input_focus_lost(event) ||
        event.type == EventType::resize || event.type == EventType::scale_changed) {
        if (ui_widget_handle_matches(g_app.message_scene.widgets.captured, q.pressed_widget))
            ui_widget_manager_capture(&g_app.message_scene.widgets, ui_widget_invalid_handle());
        q.inline_capture = false;
        g_app.downloads_pointer_tail = true;
        application_release_capture();
    } else if (input_press(event))
        q.inline_capture = false;
    return false;
}

} // namespace

bool application_downloads_window_event(const WindowEvent& event) noexcept {
    auto* q = g_app.downloads;
    if (input_press(event) && (!q || !q->active) && g_app.renderer_ready && !g_app.login.visible &&
        !g_app.image_viewer.scene.active && !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
        !application_message_forward_active() && !application_quick_switcher_active() &&
        !application_media_picker_active() && !application_search_active() &&
        !application_pinned_messages_active() &&
        !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active)) {
        if (application_downloads_save_press(client_dip_x(event), client_dip_y(event)))
            return true;
    }
    if (q && q->inline_capture && inline_press_event(*q, event))
        return true;
    if (input_press(event) && (!q || !q->active))
        g_app.downloads_pointer_tail = false;
    if (input_release(event) && g_app.downloads_pointer_tail && (!q || !q->active)) {
        g_app.downloads_pointer_tail = false;
        return true;
    }
    if (!q || !q->active)
        return false;
    if (!scene_current(q)) {
        application_downloads_close();
        return true;
    }
    if (event.type == EventType::capture_lost || event.type == EventType::cancel_input || input_focus_lost(event) ||
        event.type == EventType::resize || event.type == EventType::scale_changed) {
        revoke_capture(*q);
        return false;
    }
    if (event.type == EventType::context_menu || input_side_button(event) || input_double(event) ||
        input_horizontal_wheel(event))
        return true;
    if (!input_press(event) && !input_release(event) && event.type != EventType::pointer_move &&
        !input_vertical_wheel(event) && event.type != EventType::cursor_query)
        return false;
    if (event.type == EventType::cursor_query) {
        UiPoint cursor{};
        if (!client_cursor_dip(event, &cursor))
            return false;
        application_set_cursor(ui_download_scene_cursor(q->scene, cursor.x, cursor.y));
        return true;
    }
    if (input_vertical_wheel(event)) {
        page_step(*q, event.wheel.dy120 > 0);
        application_request_frame();
        return true;
    }
    const float x = client_dip_x(event), y = client_dip_y(event);
    if (event.type == EventType::pointer_move) {
        bool paint = false;
        ui_download_scene_set_hover(&q->scene, x, y, &paint);
        if (paint)
            application_request_frame();
        return true;
    }
    const auto target = ui_download_scene_hit_test(q->scene, x, y);
    if (input_press(event)) {
        q->pressed = target;
        q->captured = true;
        g_app.downloads_pointer_tail = false;
        q->pressed_widget = ui_widget_manager_hit_test(q->scene.widgets, x, y);
        q->pressed_viewport = q->scene.viewport;
        q->pressed_panel = q->scene.panel;
        q->pressed_layout_revision = q->scene.widgets.layout.revision;
        const auto* node = ui_widget_manager_layout_node(q->scene.widgets, q->pressed_widget);
        q->pressed_bounds = node ? node->rectangle : UiRect{};
        ui_widget_manager_capture(&q->scene.widgets, q->pressed_widget);
        ui_widget_manager_focus(&q->scene.widgets, q->pressed_widget);
        application_request_frame();
        if (g_app.window)
            window_capture_pointer(g_app.window);
        return true;
    }
    const auto pressed = q->pressed;
    const auto widget = q->pressed_widget;
    const auto held = q->scene.widgets.captured;
    const auto* node = ui_widget_manager_layout_node(q->scene.widgets, widget);
    const bool backdrop =
        !widget.generation && pressed.action == UiDownloadAction::close &&
        target.action == UiDownloadAction::close &&
        q->pressed_layout_revision == q->scene.widgets.layout.revision &&
        bytes_equal(&q->scene.viewport, &q->pressed_viewport, sizeof(q->pressed_viewport)) &&
        bytes_equal(&q->scene.panel, &q->pressed_panel, sizeof(q->pressed_panel));
    const bool captured =
        q->captured &&
        (backdrop ||
         (ui_widget_handle_matches(held, widget) && node &&
          bytes_equal(&node->rectangle, &q->pressed_bounds, sizeof(q->pressed_bounds))));
    revoke_capture(*q);
    g_app.downloads_pointer_tail = false;
    if (captured && pressed.id == target.id && pressed.action == target.action &&
        pressed.index == target.index)
        activate(*q, target);
    return true;
}
HANDLE application_downloads_notification() noexcept {
    return g_app.downloads ? g_app.downloads->picker.notification : nullptr;
}
u64 application_downloads_next_wake() noexcept {
    return g_app.downloads ? g_app.downloads->wake_at : 0;
}
void application_downloads_prepare_account_stop() noexcept {
    auto* q = g_app.downloads;
    if (!q)
        return;
    if (q->inline_capture) {
        q->inline_capture = false;
        g_app.downloads_pointer_tail = true;
        if (ui_widget_handle_matches(g_app.message_scene.widgets.captured, q->pressed_widget))
            ui_widget_manager_capture(&g_app.message_scene.widgets, ui_widget_invalid_handle());
        application_release_capture();
    }
    for (u32 i = 0; i < kDownloadJobs; ++i)
        if (q->jobs[i].occupied && !terminal(q->jobs[i].phase))
            cancel_job(*q, i);
    q->closing = true;
    application_downloads_close();
    native_file_picker_cancel(&q->picker);
    q->wake_at = deadline_after(clock_monotonic_milliseconds(), 100);
}
bool application_downloads_defer_close() noexcept {
    auto* q = g_app.downloads;
    if (!q)
        return false;
    q->close_window = true;
    return !application_downloads_release();
}
bool application_downloads_release() noexcept {
    auto* q = g_app.downloads;
    if (!q)
        return true;
    application_downloads_prepare_account_stop();
    if (!native_file_picker_release(&q->picker, 0))
        return false;
    q->picker_job = kNoDownload;
    if (q->active_job != kNoDownload && q->account_instance == g_app.account.instance &&
        g_app.account.api.download) {
        const auto result = g_app.account.api.download(
            q->account_instance, AccountDownloadOperation::release, q->jobs[q->active_job].id, nullptr);
        if (!result.released)
            return false;
    }
    ui_download_scene_release(&q->scene);
    for (auto& j : q->jobs)
        release_path(j);
    SecureZeroMemory(q, sizeof(*q));
    pages_release(q);
    g_app.downloads = nullptr;
    return true;
}

} // namespace maicord::app_detail
