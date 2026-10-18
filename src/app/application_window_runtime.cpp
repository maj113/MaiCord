#include "application_navigation_drag.hpp"
#include "application_shortcuts.hpp"
#include "application_call.hpp"
#include "application_stream.hpp"
#include "application_guild_card.hpp"
#include "application_guild_banner.hpp"
#include "application_inbox.hpp"
#include "application_cursor.hpp"
#include "application_taskbar_attention.hpp"
#include "application_unread_banner.hpp"
#include "application_downloads.hpp"
#include "application_gif_favorite_star.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/message_action_bar.hpp"
#include "maicord/ui/secure_edit.hpp"
#include "maicord/ui/secure_text_store.hpp"

#include "application_state.hpp"
#include "application_profile.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_attachments.hpp"
#include "application_forum_attachments.hpp"
#include "application_content_input.hpp"
#include "application_frame_presentation.hpp"
#include "application_input_routing.hpp"
#include "application_input_modality.hpp"
#include "application_login_auth.hpp"
#include "application_account_logout.hpp"
#include "application_login_input.hpp"
#include "application_login_ui.hpp"
#include "application_message_interaction.hpp"
#include "application_scrolling.hpp"
#include "application_shell_input.hpp"
#include "application_window.hpp"
#include "application_window_runtime.hpp"
#include "application_window_resize.hpp"
#include "application_text_form.hpp"
#include "application_forum_form.hpp"
#include "application_composer_text_input.hpp"
#include "application_button_input.hpp"
#include "application_settings.hpp"
#include "application_theme_editor.hpp"
#include "application_friends.hpp"
#include "application_plugins.hpp"
#include "application_read_state.hpp"
#include "application_notifications.hpp"
#include "application_video.hpp"
#include "application_tooltip.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_thread_browser.hpp"
#include "application_media_picker.hpp"
#include "application_quick_switcher.hpp"
#include "application_message_forward.hpp"
#include "application_surface_motion.hpp"
#include "application_poll.hpp"
#include "application_accessibility.hpp"
#include "application_desktop.hpp"
#include "application_voice_message.hpp"

#include <Windows.h>
#include <shellapi.h>

namespace maicord::app_detail {
static_assert(kWindowMessageApp == WM_APP);
namespace {
// Whether this window holds a TME_LEAVE request; cleared by WM_MOUSELEAVE.
bool g_pointer_leave_armed = false;

void update_keyboard_focus_visible(UINT message, WPARAM wparam) noexcept {
    const bool shift_f10 = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wparam == VK_F10 &&
                           (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool visible = keyboard_focus_after_message(g_app.keyboard_focus_visible, message, wparam, shift_f10);
    if (visible != g_app.keyboard_focus_visible) {
        g_app.keyboard_focus_visible = visible;
        application_request_frame();
    }
}
} // namespace

LRESULT CALLBACK application_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_ACTIVATE)
        application_animation_window_active(LOWORD(wparam) != WA_INACTIVE);
    // UI Automation (WM_GETOBJECT) and its deferred actions; observes WM_DESTROY.
    LRESULT accessibility_result = 0;
    if (application_accessibility_window_message(message, wparam, lparam, &accessibility_result))
        return accessibility_result;
    // Tray icon callbacks, Explorer restarts and launches forwarded by a second instance.
    LRESULT desktop_result = 0;
    if (application_desktop_window_message(message, wparam, lparam, &desktop_result))
        return desktop_result;
    const bool logout_blocked =
        application_account_logout_blocked() || application_account_logout_host_owned();
    if (logout_blocked) {
        // The current confirmation/retry owner receives native input before
        // any account selection, read activity, tooltip or login observer.
        LRESULT result{};
        if (application_text_form_window_message(message, wparam, lparam, &result))
            return result;
        // Generic form keys normally arrive through the application router.
        // Dispatch them directly here so locked Retry remains keyboard usable
        // while every account/login route is fenced below.
        switch (message) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_UNICHAR:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_SYSCHAR: {
            const auto kind = message == WM_KEYDOWN                           ? UiInputKind::key_down
                              : message == WM_KEYUP || message == WM_SYSKEYUP ? UiInputKind::key_up
                              : message == WM_SYSKEYDOWN                      ? UiInputKind::system_key_down
                              : message == WM_SYSCHAR ? UiInputKind::system_character
                                                      : UiInputKind::character;
            update_keyboard_focus_visible(message, wparam);
            const bool repeated =
                (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && (lparam & (1ll << 30)) != 0;
            if (application_text_form_input(kind, static_cast<u32>(wparam), repeated))
                return 0;
            break;
        }
        default:
            break;
        }
        if (message == WM_DROPFILES) {
            DragFinish(reinterpret_cast<HDROP>(wparam));
            return 0;
        }
        if (((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) &&
             (wparam == VK_F4 || wparam == VK_SPACE)) ||
            (message == WM_SYSCHAR && wparam == ' '))
            return DefWindowProcW(window, message, wparam, lparam);
        if ((message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
            (message >= WM_CUT && message <= WM_UNDO) ||
            (message >= WM_IME_STARTCOMPOSITION && message <= WM_IME_COMPOSITION) ||
            (message >= WM_IME_SETCONTEXT && message <= WM_IME_KEYUP) || message == WM_CONTEXTMENU ||
            message == WM_COMMAND || message == WM_COPYDATA || message == kBeginWebAuthnMessage ||
            message == kApplicationPhoneVerifyMessage || message == kApplicationNotificationMessage ||
            message == WM_ACTIVATE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
            message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_MOUSELEAVE)
            return 0;
        if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
            application_set_cursor(UiCursor::arrow);
            return TRUE;
        }
        // Retain the existing native paint/resize/DPI lifecycle. Other OS
        // controls (including non-client system commands) use their default
        // handler without passing through the account UI routes below.
        switch (message) {
        case WM_SETTINGCHANGE:
        case WM_ERASEBKGND:
        case WM_GETMINMAXINFO:
        case WM_ENTERSIZEMOVE:
        case WM_WINDOWPOSCHANGED:
        case WM_EXITSIZEMOVE:
        case WM_NCCALCSIZE:
        case WM_DPICHANGED:
        case WM_PAINT:
        case WM_CLOSE:
        case WM_DESTROY:
            break;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }
    if (message == kApplicationNotificationMessage) {
        application_notifications_wake(&g_app);
        return 0;
    }
    struct NotificationSelection {
        bool observe;
        ~NotificationSelection() {
            // The confirmation can commit logout during this same release.
            if (observe && !application_account_logout_blocked() &&
                !application_account_logout_host_owned())
                application_notifications_selection(&g_app);
        }
    } notification_selection{!logout_blocked &&
                             (message == WM_LBUTTONUP || message == WM_KEYUP || message == WM_ACTIVATE ||
                              message == WM_SETFOCUS || message == WM_KILLFOCUS)};
    if (!logout_blocked) {
        // Observe native input before modal handlers consume it. The read-state
        // controller independently decides whether the message pane is eligible.
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP ||
            message == WM_CHAR || message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
            message == WM_MBUTTONUP || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
            application_read_state_activity();
        update_keyboard_focus_visible(message, wparam);
        // Typing, focus loss or another button ends a multi-click sequence.
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_CHAR || message == WM_KILLFOCUS ||
            message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN)
            ui_click_tracker_reset(&g_app.click_tracker);
        // Input that can change what lies under a still pointer re-resolves
        // the cursor after the next presented frame.
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
            message == WM_MOUSEWHEEL || message == WM_KEYDOWN || message == WM_KEYUP)
            application_cursor_invalidate();
        application_tooltip_input(message, wparam, lparam);
        application_guild_banner_pointer(message, lparam);
        if (application_taskbar_attention_window_message(message))
            return 0;
        // An active splitter drag owns the pointer until release, whichever
        // panel (thread browser, search, pins) lies under it.
        if (g_app.pane_resize != PaneResizeKind::none) {
            if (message == WM_MOUSEMOVE) {
                handle_pane_resize_drag(wparam, lparam);
                return 0;
            }
            if (message == WM_LBUTTONUP) {
                end_pane_resize();
                return 0;
            }
            if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
                cancel_pane_resize();
                return 0;
            }
        }
        // A rail or channel-list drag owns the pointer and Esc until release.
        if (application_navigation_drag_window_message(message, wparam, lparam))
            return 0;
        // Leaving the client retires video hover even when a modal owns the event.
        // The observer preserves an active seek/volume capture and never consumes it.
        if (message == WM_MOUSELEAVE)
            application_video_pointer(message, lparam);
        // Hover affordances end when the pointer leaves the client, as a DOM
        // mouseleave would. Leave tracking is one shared per-window request,
        // re-armed by the first move after each WM_MOUSELEAVE.
        if (message == WM_MOUSEMOVE && !g_pointer_leave_armed) {
            TRACKMOUSEEVENT tracking{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE, window, 0};
            g_pointer_leave_armed = TrackMouseEvent(&tracking) != FALSE;
        }
        if (message == WM_MOUSELEAVE) {
            g_pointer_leave_armed = false;
            if (g_app.renderer_ready && !application_message_hover_leave())
                log_write(LogLevel::error, "ui", "message_hover_leave_failed");
        }
        if (message == WM_KILLFOCUS)
            application_video_blur();
        // A layer that consumes WM_SETCURSOR must choose a shape. If it did
        // not, present the arrow rather than leaving the previous cursor stuck.
        const u32 cursor_serial = g_application_cursor_serial;
        const auto consumed = [&](LRESULT result) noexcept -> LRESULT {
            if (message != WM_SETCURSOR)
                return result;
            if (LOWORD(lparam) != HTCLIENT)
                return DefWindowProcW(window, message, wparam, lparam);
            if (g_application_cursor_serial == cursor_serial)
                application_set_cursor(UiCursor::arrow);
            return TRUE;
        };
        LRESULT form_result = 0;
        if (g_app.text_form && application_forum_form_active() &&
            application_text_form_window_message(message, wparam, lparam, &form_result))
            return consumed(form_result);
        if (application_forum_form_window_message(message, wparam, lparam, &form_result))
            return consumed(form_result);
        if (application_text_form_window_message(message, wparam, lparam, &form_result))
            return consumed(form_result);
        if (application_theme_editor_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_settings_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_shortcuts_overlay_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_downloads_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_image_viewer_message(message, wparam, lparam))
            return consumed(0);
        if (application_message_forward_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_quick_switcher_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_inbox_window_message(message, wparam, lparam))
            return consumed(0);
        // The server tag popout traps clicks outside it while open.
        if (application_guild_card_window_message(message, wparam, lparam))
            return consumed(0);
        // Go Live preview card over voice channel rows (and LIVE badge clicks).
        if (application_stream_window_message(message, wparam, lparam))
            return consumed(0);
        // The call view covers the content column (and friends) when shown.
        if (application_call_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_friends_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_media_picker_window_message(message, wparam, lparam))
            return consumed(0);
        // Splitter grips sit on panel borders (the channel splitter is the
        // thread browser's left edge), so they win over the panels below.
        if (message == WM_LBUTTONDOWN && g_app.renderer_ready && handle_pane_resize_begin(lparam)) {
            SetFocus(window);
            return 0;
        }
        if (application_thread_browser_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_poll_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_pinned_messages_window_message(message, wparam, lparam))
            return consumed(0);
        if (message == WM_LBUTTONDOWN && !application_video_search_active() &&
            g_app.renderer.search_scene &&
            ui_search_scene_contains(*g_app.renderer.search_scene, client_dip_x(lparam),
                                     client_dip_y(lparam)))
            application_video_blur();
        if (application_video_search_active() &&
            (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE) &&
            application_video_pointer(message, lparam))
            return 0;
        if (application_search_window_message(message, wparam, lparam))
            return consumed(0);
        if (application_unread_banner_window_message(message, wparam, lparam))
            return consumed(0);
        if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE) &&
            application_video_pointer(message, lparam))
            return 0;
        if (application_button_pointer(message, lparam))
            return 0;
    }
    switch (message) {
    case WM_SETTINGCHANGE:
        application_surface_motion_refresh_system_preference();
        application_request_frame();
        break;
    case kBeginWebAuthnMessage:
        if (!application_login_run_webauthn(static_cast<u64>(wparam))) {
            log_write(LogLevel::error, "auth", "webauthn_flow_failed");
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_GETMINMAXINFO:
        application_apply_minimum_window_track(window, reinterpret_cast<MINMAXINFO*>(lparam));
        return 0;

    case WM_ENTERSIZEMOVE:
        application_begin_window_resize();
        return 0;

    case WM_WINDOWPOSCHANGED: {
        const auto* position = reinterpret_cast<const WINDOWPOS*>(lparam);
        if (position &&
            ((position->flags & SWP_NOSIZE) == 0 || (position->flags & SWP_FRAMECHANGED) != 0)) {
            application_resize_client(window);
        }
        return 0;
    }

    case WM_EXITSIZEMOVE:
        application_end_window_resize(window);
        return 0;

    case kApplicationPhoneVerifyMessage:
        if (!application_login_run_phone_verify(static_cast<u64>(wparam), static_cast<u32>(lparam))) {
            log_write(LogLevel::error, "auth", "phone_verify_flow_failed");
        }
        return 0;

    case WM_NCCALCSIZE: {
        // Prepare the composition surface before Windows commits the HWND's
        // new origin. Painting in WINDOWPOSCHANGED lets DWM display the old
        // width at the new origin for one frame when dragging the left edge.
        const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
        if (wparam && lparam && g_app.renderer_ready) {
            const auto& client = reinterpret_cast<const NCCALCSIZE_PARAMS*>(lparam)->rgrc[0];
            if (client.right > client.left && client.bottom > client.top)
                application_resize_surface(static_cast<u32>(client.right - client.left),
                                           static_cast<u32>(client.bottom - client.top));
        }
        return result;
    }

    case WM_DPICHANGED: {
        popup_scene_close(&g_app.popup_scene);
        if (!logout_blocked)
            application_plugins_menu_dismissed();
        g_app.popup_owner_kind = PopupOwnerKind::none;
        renderer_set_dpi(&g_app.renderer, HIWORD(wparam));
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }

    case WM_PAINT: {
        // The composition surface and caret are GPU-rendered. Acknowledge the
        // update region without acquiring a GDI DC that no paint path uses.
        ValidateRect(window, nullptr);
        if (g_app.frame_loop_active && !g_app.renderer.live_resize) {
            // Never block input dispatch waiting for a GIF-triggered present.
            g_app.frame_pending = true;
            return 0;
        }
        g_app.frame_pending = false;
        const bool ok = application_present_frame();
        if (!ok) {
            application_record_presentation_failure("paint_frame_rejected");
        } else {
            application_record_presentation_success();
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (g_app.renderer_ready && handle_scroll_wheel(wparam, lparam)) {
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && g_app.renderer_ready) {
            POINT point{};
            if (GetCursorPos(&point) && ScreenToClient(window, &point) &&
                set_client_cursor(point.x, point.y)) {
                return TRUE;
            }
        }
        break;

    case WM_MOUSEMOVE:
        if (application_profile_pointer(UiButtonPointerEvent::move,
                                        {client_dip_x(lparam), client_dip_y(lparam)}))
            return 0;
        if (!g_app.renderer_ready)
            break;
        update_scrollbar_hover(client_dip_x(lparam), client_dip_y(lparam));
        if (handle_reaction_viewer_hover(client_dip_x(lparam), client_dip_y(lparam)) ||
            handle_message_text_selection_drag(wparam, lparam) ||
            handle_scrollbar_drag(wparam, client_dip_y(lparam)) || handle_pane_resize_drag(wparam, lparam) ||
            handle_popup_hover(lparam) || application_login_handle_drag(wparam, lparam) ||
            handle_composer_drag(wparam, lparam))
            return 0;
        handle_message_hover(lparam);
        if (message_action_bar_hit_test(g_app.message_action_bar, client_dip_x(lparam), client_dip_y(lparam))
                .inside)
            return 0;
        if (g_app.login.visible || handle_shell_hover(lparam) ||
            handle_navigation_hover(lparam) || handle_composer_hover(lparam) || handle_member_hover(lparam))
            return 0;
        handle_content_hover(lparam);
        break;

    case WM_LBUTTONUP:
        if (application_profile_pointer(UiButtonPointerEvent::release,
                                        {client_dip_x(lparam), client_dip_y(lparam)}))
            return 0;
        if (g_app.message_text_pointer_selection) {
            end_message_text_selection();
            return 0;
        }
        if (g_app.scrollbar_drag != ScrollbarDragKind::none) {
            end_scrollbar_drag();
            return 0;
        }
        if (g_app.pane_resize != PaneResizeKind::none) {
            end_pane_resize();
            return 0;
        }
        if (g_app.login.pointer_selection != LoginControl::none) {
            application_login_end_pointer_selection();
            return 0;
        }
        if (g_app.message_composer_pointer_selection) {
            end_message_composer_pointer_selection();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        application_video_cancel_pointer();
        application_profile_pointer(UiButtonPointerEvent::cancel, {});
        g_app.scrollbar_drag = ScrollbarDragKind::none;
        g_app.scrollbar_thumb_grab_offset = 0.0f;
        end_pane_resize();
        g_app.login.pointer_selection = LoginControl::none;
        ui_widget_manager_capture(&g_app.login_scene.widgets, ui_widget_invalid_handle());
        g_app.message_text_pointer_selection = false;
        end_message_composer_pointer_selection();
        return 0;

    case WM_LBUTTONDOWN:
        if (application_profile_pointer(UiButtonPointerEvent::press,
                                        {client_dip_x(lparam), client_dip_y(lparam)}))
            return 0;
        ui_button_keyboard_cancel(&g_app.button_keyboard);
        if (g_app.renderer_ready &&
            handle_reaction_viewer_click(client_dip_x(lparam), client_dip_y(lparam))) {
            SetFocus(window);
            return 0;
        }
        if (g_app.message_text_selection.valid && !clear_message_text_selection(true)) {
            log_write(LogLevel::warning, "ui", "message_text_selection_clear_failed");
        }
        if (!g_app.renderer_ready)
            break;
        if (handle_popup_click(wparam, lparam) || handle_message_action_bar_click(lparam) ||
            application_login_handle_click(lparam) ||
            handle_scrollbar_begin(client_dip_x(lparam), client_dip_y(lparam))) {
            SetFocus(window);
            return 0;
        }
        focus_context_at(client_dip_x(lparam), client_dip_y(lparam));
        application_request_frame();
        if (handle_shell_click(lparam) || handle_composer_click(lparam)) {
            SetFocus(window);
            return 0;
        }
        if (application_gif_favorite_star_press(client_dip_x(lparam), client_dip_y(lparam)) ||
            application_message_media_reveal_at(client_dip_x(lparam), client_dip_y(lparam)) ||
            application_image_viewer_open_at(client_dip_x(lparam), client_dip_y(lparam)) ||
            handle_member_click(lparam) || handle_message_click(lparam) || handle_content_click(lparam) ||
            handle_navigation_click(lparam))
            return 0;
        if (begin_message_text_selection(lparam)) {
            SetFocus(window);
            return 0;
        }
        break;

    case WM_LBUTTONDBLCLK:
        if (application_profile_double_click({client_dip_x(lparam), client_dip_y(lparam)}))
            return 0;
        if (route_application_input(UiInputKind::primary_double_click, 0)) {
            return 0;
        }
        if (g_app.renderer_ready && application_login_handle_click(lparam)) {
            SetFocus(window);
            return 0;
        }
        if (g_app.renderer_ready && handle_composer_double_click(lparam)) {
            SetFocus(window);
            return 0;
        }
        // Message text: double click selects the word under the pointer.
        if (g_app.renderer_ready && begin_message_text_selection(lparam, true)) {
            SetFocus(window);
            return 0;
        }
        break;

    case WM_SYSKEYDOWN:
        if (route_application_input(UiInputKind::system_key_down, static_cast<u32>(wparam),
                                    (lparam & (1ll << 30)) != 0)) {
            return 0;
        }
        break;

    case WM_SYSCHAR:
        if (route_application_input(UiInputKind::system_character, static_cast<u32>(wparam))) {
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (application_quick_switcher_input(UiInputKind::key_down, static_cast<u32>(wparam),
                                             (lparam & (1ll << 30)) != 0))
            return 0;
        if (application_video_key(static_cast<UINT>(wparam), (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                                  (lparam & (1ll << 30)) != 0))
            return 0;
        if (route_application_input(UiInputKind::key_down, static_cast<u32>(wparam),
                                    (lparam & (1ll << 30)) != 0)) {
            return 0;
        }
        break;

    case WM_KEYUP:
        if (route_application_input(UiInputKind::key_up, static_cast<u32>(wparam)))
            return 0;
        break;

    case WM_CANCELMODE:
        application_video_cancel_pointer();
        ui_button_keyboard_cancel(&g_app.button_keyboard);
        break;

    case WM_CHAR:
        if (route_application_input(UiInputKind::character, static_cast<u32>(wparam))) {
            return 0;
        }
        break;

    case WM_SETFOCUS:
        if (g_app.renderer_ready && g_app.login.visible) {
            secure_text_store_set_window_focus(&g_app.login.text_store, true);
            application_reset_login_caret_timing(g_app.renderer.login_edit.active);
            application_request_frame();
        } else if (g_app.renderer_ready && g_app.account_ui && g_app.message_composer.focused) {
            reset_message_composer_caret_timing(true);
            application_request_frame();
        }
        return 0;

    case WM_KILLFOCUS:
        if (!wparam || GetWindow(reinterpret_cast<HWND>(wparam), GW_OWNER) != window) {
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
        return 0;

    case WM_CONTEXTMENU: {
        if (!g_app.renderer_ready) {
            break;
        }
        if (g_app.reaction_viewer.active) {
            return 0;
        }
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        if (point.x == -1 && point.y == -1) {
            if (!open_focused_context_popup())
                log_write(LogLevel::warning, "ui", "focused_context_popup_open_failed");
            return 0;
        }
        if (!ScreenToClient(window, &point)) {
            return 0;
        }
        if (!open_context_popup(point.x, point.y)) {
            log_write(LogLevel::warning, "ui", "context_popup_open_failed");
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE)
            application_read_state_activity();
        // Never while minimized: the taskbar's SC_RESTORE is queued behind
        // this activation, and the refresh's COM call would discard it.
        if (desktop_activation_refresh_allowed(wparam))
            application_notifications_settings_refresh(&g_app);
        return 0;

    case WM_CLOSE: {
        // Minimize to tray: the close button hides the window instead.
        if (application_desktop_close_to_tray())
            return 0;
        const bool downloads_pending = application_downloads_defer_close();
        const bool attachments_pending = application_attachments_defer_close();
        const bool forum_pending = application_forum_attachments_defer_close();
        if (downloads_pending || attachments_pending || forum_pending)
            return 0;
        break;
    }
    case WM_DROPFILES:
        if (!application_drop_attachments(reinterpret_cast<HANDLE>(wparam)))
            log_write(LogLevel::error, "attachment", "file_drop_accept_failed");
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace maicord::app_detail
