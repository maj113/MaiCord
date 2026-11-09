// The portable half of the window procedure: every window event of the main window, in the order the overlays and the
// shell take it (window_runtime.hpp). The Windows-only half (state observer, caption frame, UI Automation, shell
// integration) lives in window_runtime.cpp.
#include "../account/dm_profile.hpp"
#include "../account/application_account_module.hpp"
#include "../plugins/pipeline_host.hpp"
#include "../effects/effects.hpp"
#include "../packages/packages.hpp"
#include "navigation_drag.hpp"
#include "content_point.hpp"
#include "shortcuts.hpp"
#include "../voice/call.hpp"
#include "../voice/stream.hpp"
#include "../chat/private_header.hpp"
#include "../voice/go_live.hpp"
#include "../guild/card.hpp"
#include "../media/expression_card.hpp"
#include "../guild/banner.hpp"
#include "../chat/inbox.hpp"
#include "cursor.hpp"
#include "../chat/unread_banner.hpp"
#include "../media/downloads.hpp"
#include "../media/gif_favorite_star.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/chat/messages/action_bar.hpp"
#include "maicord/ui/text/secure_edit.hpp"
#include "maicord/ui/text/secure_text_store.hpp"

#include "state.hpp"
#include "../account/profile.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/composer/input.hpp"
#include "../chat/composer/attachments.hpp"
#include "../chat/threads/forum_attachments.hpp"
#include "content_input.hpp"
#include "frame_presentation.hpp"
#include "input_routing.hpp"
#include "input_modality.hpp"
#include "../account/login/auth.hpp"
#include "../account/application_account_logout.hpp"
#include "../account/login/input.hpp"
#include "../account/login/ui.hpp"
#include "../chat/messages/interaction.hpp"
#include "scrolling.hpp"
#include "input.hpp"
#include "window.hpp"
#include "overlay_input.hpp"
#include "window_state.hpp"
#include "window_runtime.hpp"
#include "window_resize.hpp"
#include "../chat/composer/text_form.hpp"
#include "../chat/threads/forum_form.hpp"
#include "../chat/composer/text_input.hpp"
#include "button_input.hpp"
#include "../settings/settings.hpp"
#include "../settings/modal.hpp"
#include "../settings/account_settings.hpp"
#include "../settings/theme_editor.hpp"
#include "../account/friends.hpp"
#include "../plugins/plugins.hpp"
#include "../chat/read_state.hpp"
#include "../notifications/notifications.hpp"
#include "../media/video.hpp"
#include "tooltip.hpp"
#include "notice.hpp"
#include "../chat/search/search.hpp"
#include "../chat/messages/pinned_messages.hpp"
#include "../chat/threads/browser.hpp"
#include "../media/picker.hpp"
#include "quick_switcher.hpp"
#include "../chat/messages/forward.hpp"
#include "surface_motion.hpp"
#include "../chat/polls/poll.hpp"
#include "../accessibility/accessibility.hpp"
#include "caption.hpp"
#include "../notifications/desktop.hpp"
#include "../voice/application_voice_message.hpp"

#include "maicord/platform/window/key.hpp"

namespace maicord::app_detail {
namespace {
// Input never lands on a closing surface: its exit (a modal layer's or a settings dialog's) ends first.
void finish_exits() noexcept {
    application_surface_motion_finish_exits();
    settings_modal_finish_exit();
}
// The sink is the only place window state changes are observed: position changes carry every
// show/hide/minimize/restore/resize; activation, focus and scale have their own events. It runs first, before any
// handler reads the flags.
void observe_window_state(const WindowEvent& event) noexcept {
    switch (event.type) {
    case EventType::context_menu:
        finish_exits();
        break;
    case EventType::key_down:
    case EventType::pointer_down:
    case EventType::wheel:
        finish_exits();
        application_effects_note_input();
        break;
    case EventType::key_up:
    case EventType::character:
    case EventType::pointer_move:
    case EventType::pointer_up:
        application_effects_note_input();
        break;
    case EventType::state_changed:
        // Creation-time events see a window not yet handed back to the application.
        if (g_app.window)
            application_window_observe_position(g_app.window);
        break;
    case EventType::activation_changed:
        g_window_state.foreground = event.focus.gained;
        break;
    case EventType::focus_changed:
        g_window_state.keyboard_focus = event.focus.gained;
        break;
    case EventType::scale_changed:
        g_window_state.dpi = input_dpi(event);
        break;
    default:
        break;
    }
}
void update_keyboard_focus_visible(const WindowEvent& event) noexcept {
    const bool shift_f10 = input_key_down(event) && event.key.key == key::f10 && application_shift_down();
    const bool visible = keyboard_focus_after_event(g_app.keyboard_focus_visible, event, shift_f10);
    if (visible != g_app.keyboard_focus_visible) {
        g_app.keyboard_focus_visible = visible;
        application_request_frame();
    }
}
// Every event reaches the application here; false leaves it to the backend's default (the system's default
// procedure on Windows).
bool chain(const WindowEvent& event, WindowEventResult* answer) noexcept {
    const EventType type = event.type;
    // A query: where a pixel of the frame the app draws lies.
    if (type == EventType::hit_test) {
        answer->value = static_cast<i32>(application_caption_hit(event.pointer.x, event.pointer.y));
        return true;
    }
    if (application_video_discard_character(event))
        return true;
    // The bar's own items, and the bar's follow-up of scale and size changes, before content coordinates apply.
    if (application_caption_event(event))
        return true;
    // The backend sized the client before the window's new origin is committed: prepare the composition surface for
    // it, so a left-edge drag never shows the old width at the new origin. The content is what lies below the bar.
    if (type == EventType::resize && (event.flags & ev_provisional)) {
        const u32 caption = g_app.renderer.caption_height;
        if (g_app.renderer_ready && event.resize.height_px > caption)
            application_resize_surface(event.resize.width_px, event.resize.height_px - caption);
        return true;
    }
    if (type == EventType::activation_changed)
        application_animation_window_active(event.focus.gained);
    // The same event with the position in content pixels: the caption's rows come off the client point once.
    WindowEvent content = event;
    if (g_app.renderer.caption_height && input_has_position(event) && !(event.flags & ev_keyboard)) {
        const auto caption = static_cast<float>(g_app.renderer.caption_height);
        if (type == EventType::wheel)
            content.wheel.y -= caption;
        else if (type == EventType::pointer_move || type == EventType::pointer_down || type == EventType::pointer_up ||
                 type == EventType::cursor_query || type == EventType::context_menu)
            content.pointer.y -= caption;
    }
    const bool system_key = (event.flags & ev_system) != 0;
    const bool keyboard = type == EventType::key_down || type == EventType::key_up || type == EventType::character;
    const bool logout_blocked = application_input_blocked();
    if (logout_blocked) {
        // The current confirmation/retry owner receives native input before
        // any account selection, read activity, tooltip or login observer.
        if (application_text_form_window_event(content))
            return true;
        // Generic form keys normally arrive through the application router.
        // Dispatch them directly here so locked Retry remains keyboard usable
        // while every account/login route is fenced below.
        if (keyboard) {
            const auto kind = type == EventType::key_down ? (system_key ? UiInputKind::system_key_down : UiInputKind::key_down)
                              : type == EventType::key_up ? UiInputKind::key_up
                              : system_key                ? UiInputKind::system_character
                                                          : UiInputKind::character;
            update_keyboard_focus_visible(content);
            const bool repeated = type == EventType::key_down && input_repeated(event);
            const u32 code = type == EventType::character ? event.character.unit : event.key.key;
            if (application_text_form_input(kind, code, repeated))
                return true;
        }
        if (type == EventType::drop_files) {
            dropped_files_release(event.drop.files);
            return true;
        }
        if ((system_key && (type == EventType::key_down || type == EventType::key_up) &&
             (event.key.key == key::f4 || event.key.key == key::space)) ||
            (system_key && type == EventType::character && event.character.unit == ' '))
            return false;
        if (keyboard || input_pointer(event) || type == EventType::context_menu ||
            (type == EventType::wake && event.wake.cause != wake_poke) || type == EventType::activation_changed ||
            type == EventType::focus_changed || type == EventType::capture_lost || type == EventType::cancel_input ||
            type == EventType::pointer_leave)
            return true;
        if (type == EventType::cursor_query) {
            application_set_cursor(UiCursor::arrow);
            return true;
        }
        // Retain the existing native paint/resize/DPI lifecycle. Other OS
        // controls (including non-client system commands) use their default
        // handler without passing through the account UI routes below.
        switch (type) {
        case EventType::settings_changed:
        case EventType::size_limits:
        case EventType::resize_begin:
        case EventType::state_changed:
        case EventType::resize_end:
        case EventType::scale_changed:
        case EventType::paint:
        case EventType::close_requested:
        case EventType::destroyed:
            break;
        default:
            return false;
        }
    }
    if (type == EventType::wake && event.wake.cause == wake_notifications) {
        application_notifications_wake(&g_app);
        return true;
    }
    struct NotificationSelection {
        bool observe;
        ~NotificationSelection() {
            // The confirmation can commit logout during this same release.
            if (observe && !application_account_logout_blocked() &&
                !application_account_logout_host_owned())
                application_notifications_selection(&g_app);
        }
    } notification_selection{!logout_blocked && (input_release(event) || input_plain_key_up(event) ||
                                                 type == EventType::activation_changed ||
                                                 type == EventType::focus_changed)};
    // Every scrollbar's hover and drag look follows the pointer once the message's owner handled it.
    struct ScrollbarPointer {
        const WindowEvent* event;
        ~ScrollbarPointer() {
            if (event)
                track_scrollbar_pointer(*event);
        }
    } scrollbar_pointer{logout_blocked ? nullptr : &content};
    if (!logout_blocked) {
        // Observe native input before modal handlers consume it. The read-state
        // controller independently decides whether the message pane is eligible.
        if (type == EventType::key_down || input_plain_key_up(event) ||
            (type == EventType::character && !system_key) ||
            (type == EventType::pointer_up && event.button <= static_cast<u8>(PointerButton::middle)) ||
            type == EventType::wheel)
            application_read_state_activity();
        update_keyboard_focus_visible(content);
        // Typing, focus loss or another button ends a multi-click sequence.
        if (type == EventType::key_down || (type == EventType::character && !system_key) || input_focus_lost(event) ||
            input_press(event, PointerButton::right) || input_press(event, PointerButton::middle))
            ui_click_tracker_reset(&g_app.click_tracker);
        // Input that can change what lies under a still pointer re-resolves
        // the cursor after the next presented frame.
        if (input_press(event) || input_release(event) || input_release(event, PointerButton::right) ||
            input_vertical_wheel(event) || input_plain_key_down(event) || input_plain_key_up(event))
            application_cursor_invalidate();
        application_tooltip_input(content);
        application_guild_banner_pointer(content);
        // An active splitter drag owns the pointer until release, whichever
        // panel (thread browser, search, pins) lies under it.
        if (g_app.pane_resize != PaneResizeKind::none) {
            if (type == EventType::pointer_move) {
                handle_pane_resize_drag(content);
                return true;
            }
            if (input_release(event)) {
                end_pane_resize();
                return true;
            }
            if (input_plain_key_down(event) && event.key.key == key::escape) {
                cancel_pane_resize();
                return true;
            }
        }
        // A rail or channel-list drag owns the pointer and Esc until release.
        if (application_navigation_drag_window_event(content))
            return true;
        // Leaving the client retires video hover even when a modal owns the event.
        // The observer preserves an active seek/volume capture and never consumes it.
        if (type == EventType::pointer_leave) {
            application_video_pointer(content);
            if (g_app.renderer_ready && !application_message_hover_leave())
                log_write(LogLevel::error, "ui", "message_hover_leave_failed");
            // The DM profile's name effect animates only under the pointer.
            application_dm_profile_hover({-1, -1});
        }
        if (input_focus_lost(event))
            application_video_blur();
        // The image magnifier watches the pointer, Ctrl and focus; only the wheel over a shown lens is its own.
        if (g_app.renderer_ready && application_image_zoom_message(content))
            return true;
        // A layer that consumes a cursor query must choose a shape. If it did
        // not, present the arrow rather than leaving the previous cursor stuck.
        const u32 cursor_serial = g_application_cursor_serial;
        const auto consumed = [&]() noexcept -> bool {
            if (type == EventType::cursor_query && g_application_cursor_serial == cursor_serial)
                application_set_cursor(UiCursor::arrow);
            return true;
        };
        if (g_app.text_form && application_forum_form_active() && application_text_form_window_event(content))
            return consumed();
        if (application_forum_form_window_event(content))
            return consumed();
        if (application_text_form_window_event(content))
            return consumed();
        if (application_theme_editor_window_event(content))
            return consumed();
        if (application_settings_window_event(content))
            return consumed();
        if (application_shortcuts_overlay_window_event(content))
            return consumed();
        if (application_downloads_window_event(content))
            return consumed();
        if (application_go_live_modal_window_event(content))
            return consumed();
        if (application_image_viewer_event(content))
            return consumed();
        if (application_message_forward_window_event(content))
            return consumed();
        if (application_quick_switcher_window_event(content))
            return consumed();
        if (application_inbox_window_event(content))
            return consumed();
        // The server tag popout traps clicks outside it while open.
        if (application_guild_card_window_event(content))
            return consumed();
        // The emoji/sticker popout; a click outside closes it and still acts.
        if (application_expression_card_window_event(content))
            return consumed();
        // Go Live preview card over voice channel rows (and LIVE badge clicks).
        if (application_stream_window_event(content))
            return consumed();
        // The DM header's avatar and name: click for the full profile, hover for the names popout.
        if (application_private_header_window_event(content))
            return consumed();
        // The call view covers the content column (and friends) when shown.
        if (application_call_window_event(content))
            return consumed();
        if (application_friends_window_event(content))
            return consumed();
        if (application_media_picker_window_event(content))
            return consumed();
        // Splitter grips sit on panel borders (the channel splitter is the
        // thread browser's left edge), so they win over the panels below.
        if (input_press(content) && g_app.renderer_ready && handle_pane_resize_begin(content)) {
            application_focus_window();
            return true;
        }
        if (application_thread_browser_window_event(content))
            return consumed();
        if (application_poll_window_event(content))
            return consumed();
        if (application_pinned_messages_window_event(content))
            return consumed();
        if (input_press(content) && !application_video_search_active() &&
            g_app.renderer.search_scene &&
            ui_search_scene_contains(*g_app.renderer.search_scene, client_dip_x(content),
                                     client_dip_y(content)))
            application_video_blur();
        if (application_video_search_active() &&
            (input_press(content) || input_release(content) || input_move(content)) &&
            application_video_pointer(content))
            return true;
        if (application_search_window_event(content))
            return consumed();
        if (application_unread_banner_window_event(content))
            return consumed();
        if ((input_press(content) || input_release(content) || input_move(content)) &&
            application_video_pointer(content))
            return true;
        if (application_button_pointer(content))
            return true;
    }
    switch (type) {
    case EventType::settings_changed:
        application_surface_motion_refresh_system_preference();
        // Windows' app mode switched between light and dark: an effect's appearance input glides to it.
        if (event.settings.areas & settings_scheme)
            application_effects_set_system_dark(platform_settings_read().dark_scheme);
        application_request_frame();
        break;
    case EventType::wake:
        if (event.wake.cause == wake_begin_webauthn) {
#if !defined(_WIN32)
            // The ceremony's worker wakes the loop with b = 1 (the key wants a PIN) or 2 (it ended): the same cause as the begin.
            if (event.wake.b != 0) {
                // A ceremony of the account settings (a key being registered or verified) carries its own token.
                if (application_account_settings_webauthn_progress(event.wake.a, event.wake.b))
                    return true;
                if (!application_login_webauthn_progress(event.wake.a, event.wake.b))
                    log_write(LogLevel::error, "auth", "webauthn_progress_failed");
                return true;
            }
#endif
            if (!application_login_run_webauthn(event.wake.a)) {
                log_write(LogLevel::error, "auth", "webauthn_flow_failed");
            }
            return true;
        }
        if (event.wake.cause == wake_phone_verify) {
            if (!application_login_run_phone_verify(event.wake.a, static_cast<u32>(event.wake.b))) {
                log_write(LogLevel::error, "auth", "phone_verify_flow_failed");
            }
            return true;
        }
        break;
    case EventType::size_limits:
        application_minimum_client_size(&answer->width_px, &answer->height_px);
        return true;
    case EventType::resize_begin:
        application_begin_window_resize();
        return true;
    case EventType::state_changed:
        if (event.window_state.changed & state_geometry)
            application_resize_client();
        return true;
    case EventType::resize_end:
        application_end_window_resize();
        return true;
    case EventType::scale_changed:
        popup_scene_close(&g_app.popup_scene);
        if (!logout_blocked)
            application_plugins_menu_dismissed();
        g_app.popup_owner_kind = PopupOwnerKind::none;
        renderer_set_dpi(&g_app.renderer, application_zoomed_dpi(input_dpi(event)));
        return true;
    case EventType::paint: {
        if (g_app.frame_loop_active && !g_app.renderer.live_resize) {
            // Never block input dispatch waiting for a GIF-triggered present.
            g_app.frame_pending = true;
            return true;
        }
        g_app.frame_pending = false;
        const bool ok = application_present_frame();
        if (!ok) {
            application_record_presentation_failure("paint_frame_rejected");
        } else {
            application_record_presentation_success();
        }
        return true;
    }
    case EventType::wheel:
        if (g_app.renderer_ready && handle_scroll_wheel(content))
            return true;
        break;
    case EventType::cursor_query:
        if (g_app.renderer_ready) {
            POINT point{};
            if (application_cursor_content(&point) && set_client_cursor(point.x, point.y))
                return true;
        }
        break;
    case EventType::pointer_move:
        if (application_profile_pointer(UiButtonPointerEvent::move, {client_dip_x(content), client_dip_y(content)}))
            return true;
        if (!g_app.renderer_ready)
            break;
        update_scrollbar_hover(client_dip_x(content), client_dip_y(content));
        if (handle_reaction_viewer_hover(client_dip_x(content), client_dip_y(content)) ||
            handle_message_text_selection_drag(content) ||
            handle_scrollbar_drag(input_held(content), client_dip_y(content)) || handle_pane_resize_drag(content) ||
            handle_popup_hover(content) || application_login_handle_drag(content) || handle_composer_drag(content))
            return true;
        handle_message_hover(content);
        if (message_action_bar_hit_test(g_app.message_action_bar, client_dip_x(content), client_dip_y(content))
                .inside)
            return true;
        // The DM profile panel tracks the pointer leaving it as well.
        if (!g_app.login.visible && application_dm_profile_hover({client_dip_x(content), client_dip_y(content)}))
            return true;
        if (g_app.login.visible || handle_shell_hover(content) || handle_navigation_hover(content) ||
            handle_composer_hover(content) || handle_member_hover(content))
            return true;
        handle_content_hover(content);
        break;
    case EventType::pointer_up:
        if (!input_release(event))
            break;
        if (application_profile_pointer(UiButtonPointerEvent::release,
                                        {client_dip_x(content), client_dip_y(content)}))
            return true;
        // A click on an emoji or sticker without a drag opens its popout; the
        // release still ends a text selection begun on it.
        if (g_app.renderer_ready)
            application_expression_card_pointer_up({client_dip_x(content), client_dip_y(content)});
        if (g_app.message_text_pointer_selection) {
            end_message_text_selection();
            return true;
        }
        if (g_app.scrollbar_drag != ScrollbarDragKind::none) {
            end_scrollbar_drag();
            return true;
        }
        if (g_app.pane_resize != PaneResizeKind::none) {
            end_pane_resize();
            return true;
        }
        if (g_app.login.pointer_selection != LoginControl::none) {
            application_login_end_pointer_selection();
            return true;
        }
        if (g_app.message_composer_pointer_selection) {
            end_message_composer_pointer_selection();
            return true;
        }
        break;
    case EventType::capture_lost:
        application_video_cancel_pointer();
        application_profile_pointer(UiButtonPointerEvent::cancel, {});
        g_app.scrollbar_drag = ScrollbarDragKind::none;
        g_app.scrollbar_thumb_grab_offset = 0.0f;
        end_pane_resize();
        g_app.login.pointer_selection = LoginControl::none;
        ui_widget_manager_capture(&g_app.login_scene.widgets, ui_widget_invalid_handle());
        g_app.message_text_pointer_selection = false;
        end_message_composer_pointer_selection();
        return true;
    case EventType::pointer_down:
        if (input_press(event)) {
            if (application_profile_pointer(UiButtonPointerEvent::press,
                                            {client_dip_x(content), client_dip_y(content)}))
                return true;
            ui_button_keyboard_cancel(&g_app.button_keyboard);
            if (g_app.renderer_ready &&
                handle_reaction_viewer_click(client_dip_x(content), client_dip_y(content))) {
                application_focus_window();
                return true;
            }
            if (g_app.message_text_selection.valid && !clear_message_text_selection(true)) {
                log_write(LogLevel::warning, "ui", "message_text_selection_clear_failed");
            }
            if (!g_app.renderer_ready)
                break;
            if (handle_popup_click(content) || handle_message_action_bar_click(content) ||
                application_login_handle_click(content) ||
                application_notice_click(client_dip_x(content), client_dip_y(content)) ||
                handle_scrollbar_begin(client_dip_x(content), client_dip_y(content))) {
                application_focus_window();
                return true;
            }
            focus_context_at(client_dip_x(content), client_dip_y(content));
            application_request_frame();
            if (handle_shell_click(content) || handle_composer_click(content)) {
                application_focus_window();
                return true;
            }
            application_expression_card_pointer_down({client_dip_x(content), client_dip_y(content)});
            // An attachment's ALT badge opens its description's popout instead of the media under it.
            if (application_alt_text_toggle(client_dip_x(content), client_dip_y(content)) ||
                application_gif_favorite_star_press(client_dip_x(content), client_dip_y(content)) ||
                application_message_media_reveal_at(client_dip_x(content), client_dip_y(content)) ||
                application_image_viewer_open_at(client_dip_x(content), client_dip_y(content)) ||
                application_dm_profile_click({client_dip_x(content), client_dip_y(content)}) ||
                handle_member_click(content) || handle_message_click(content) || handle_content_click(content) ||
                handle_navigation_click(content) || application_plugins_message_click(content, false, 1))
                return true;
            if (begin_message_text_selection(content)) {
                application_focus_window();
                return true;
            }
        } else if (input_press(event, PointerButton::middle)) {
            if (g_app.renderer_ready && application_plugins_message_click(content, true, 1))
                return true;
        } else if (input_double(event)) {
            if (application_profile_double_click({client_dip_x(content), client_dip_y(content)}))
                return true;
            if (route_application_input(UiInputKind::primary_double_click, 0))
                return true;
            if (g_app.renderer_ready && application_login_handle_click(content)) {
                application_focus_window();
                return true;
            }
            if (g_app.renderer_ready && handle_composer_double_click(content)) {
                application_focus_window();
                return true;
            }
            // A plugin may take a double click on a message before the word selection does.
            if (g_app.renderer_ready && application_plugins_message_click(content, false, 2))
                return true;
            // Message text: double click selects the word under the pointer.
            if (g_app.renderer_ready && begin_message_text_selection(content, true)) {
                application_focus_window();
                return true;
            }
        }
        break;
    case EventType::key_down: {
        const u32 code = event.key.key;
        const bool repeated = input_repeated(event);
        if (system_key) {
            if (route_application_input(UiInputKind::system_key_down, code, repeated))
                return true;
            break;
        }
        if (application_quick_switcher_input(UiInputKind::key_down, code, repeated))
            return true;
        if (application_video_key(code, application_shift_down(), repeated))
            return true;
        if (route_application_input(UiInputKind::key_down, code, repeated))
            return true;
        break;
    }
    case EventType::key_up:
        if (!system_key && route_application_input(UiInputKind::key_up, event.key.key))
            return true;
        break;
    case EventType::cancel_input:
        application_video_cancel_pointer();
        ui_button_keyboard_cancel(&g_app.button_keyboard);
        break;
    case EventType::character:
        if (route_application_input(system_key ? UiInputKind::system_character : UiInputKind::character,
                                    event.character.unit))
            return true;
        break;
    case EventType::focus_changed:
        if (event.focus.gained) {
            if (g_app.renderer_ready && g_app.login.visible) {
                secure_text_store_set_window_focus(&g_app.login.text_store, true);
                application_reset_login_caret_timing(g_app.renderer.login_edit.active);
                application_request_frame();
            } else if (g_app.renderer_ready && g_app.account_ui && g_app.message_composer.focused) {
                reset_message_composer_caret_timing(true);
                application_request_frame();
            }
            return true;
        }
        if ((event.flags & ev_owned_focus) == 0) {
            if (g_app.popup_scene.active)
                dismiss_popup();
            else
                application_profile_close();
            // As in the official client, a recording stops when the client stops being active.
            application_voice_message_deactivated();
        }
        g_app.composer_menu_held_key = 0;
        application_composer_text_input_suspend();
        ui_button_keyboard_cancel(&g_app.button_keyboard);
        application_login_end_pointer_selection();
        end_message_composer_pointer_selection();
        end_message_text_selection();
        secure_text_store_set_window_focus(&g_app.login.text_store, false);
        if (g_app.renderer_ready) {
            application_reset_login_caret_timing(false);
            reset_message_composer_caret_timing(false);
            application_request_frame();
        }
        secure_edit_cancel_composition(&g_app.login.login);
        secure_edit_cancel_composition(&g_app.login.password);
        secure_edit_cancel_composition(&g_app.login.mfa);
        return true;
    case EventType::ime_preedit:
    case EventType::ime_commit:
    case EventType::ime_delete_surrounding:
        return secure_text_store_ime_event(event);
    case EventType::context_menu: {
        if (!g_app.renderer_ready)
            break;
        if (g_app.reaction_viewer.active)
            return true;
        if ((event.flags & ev_keyboard) != 0) {
            if (!open_focused_context_popup())
                log_write(LogLevel::warning, "ui", "focused_context_popup_open_failed");
            return true;
        }
        if (!open_context_popup(static_cast<i32>(content.pointer.x), static_cast<i32>(content.pointer.y))) {
            log_write(LogLevel::warning, "ui", "context_popup_open_failed");
        }
        return true;
    }
    case EventType::activation_changed:
        application_account_report_focus(event.focus.gained && (event.flags & ev_minimized) == 0);
        if (event.focus.gained)
            application_read_state_activity();
        // Never while minimized: the taskbar's SC_RESTORE is queued behind
        // this activation, and the refresh's COM call would discard it.
        if (event.focus.gained && (event.flags & ev_minimized) == 0)
            application_notifications_settings_refresh(&g_app);
        return true;
    case EventType::close_requested: {
        // Minimize to tray: the close button hides the window instead.
        if (application_desktop_close_to_tray())
            return true;
        const bool downloads_pending = application_downloads_defer_close();
        const bool attachments_pending = application_attachments_defer_close();
        const bool forum_pending = application_forum_attachments_defer_close();
        if (downloads_pending || attachments_pending || forum_pending)
            return true;
        break;
    }
    case EventType::drop_files:
        if (application_packages_drop(event.drop.files))
            return true;
        if (!application_drop_attachments(event.drop.files))
            log_write(LogLevel::error, "attachment", "file_drop_accept_failed");
        return true;
    case EventType::destroyed:
        return true;
    default:
        break;
    }
    return false;
}

} // namespace

bool application_input_blocked() noexcept {
    return application_account_logout_blocked() || application_account_logout_host_owned();
}

void application_window_event(void*, const WindowEvent& event, WindowEventResult* result) noexcept {
    observe_window_state(event);
#if !defined(_WIN32)
    // The AT-SPI requests the accessibility host deferred arrive as wake events (UI Automation's are window messages that
    // the Windows shell hook takes before this sink).
    if (application_accessibility_window_event(event)) {
        result->handled = true;
        return;
    }
#endif
    result->handled = chain(event, result);
}

} // namespace maicord::app_detail
