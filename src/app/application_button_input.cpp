#include "application_button_input.hpp"
#include "application_state.hpp"
#include "application_composer_input.hpp"
#include "application_composer.hpp"
#include "application_shell_input.hpp"
#include "application_thread_header.hpp"
#include "application_voice_message.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/settings_scene.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
bool application_button_pointer(UINT message, LPARAM lparam) noexcept {
    UiButtonPointerEvent event{};
    if (message == WM_MOUSEMOVE)
        event = UiButtonPointerEvent::move;
    else if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK)
        // Windows replaces the second down in a rapid click pair with DBLCLK.
        // Button activation still belongs to each matching press/release pair.
        event = UiButtonPointerEvent::press;
    else if (message == WM_LBUTTONUP)
        event = UiButtonPointerEvent::release;
    else if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS)
        event = UiButtonPointerEvent::cancel;
    else
        return false;
    auto* captured_owner = g_app.button_pointer_owner;
    const bool settings_active = g_app.renderer.settings_scene && g_app.renderer.settings_scene->active;
    const bool settings_pointer = settings_active && event != UiButtonPointerEvent::cancel;
    const bool blocked = !g_app.renderer_ready || !g_app.account_ui || g_app.login.visible ||
                         settings_active || g_app.popup_scene.active || g_app.reaction_viewer.active ||
                         g_app.text_form ||
                         (g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
    if (blocked && !captured_owner)
        return settings_pointer;
    if (blocked)
        event = UiButtonPointerEvent::cancel;
    const bool cancel = event == UiButtonPointerEvent::cancel;
    UiWidgetManager* owners[]{&g_app.message_composer_scene.widgets, &g_app.shell_scene.widgets};
    const UiPoint point{client_dip_x(lparam), client_dip_y(lparam)};
    if (!captured_owner || captured_owner == &g_app.shell_scene.widgets) {
        const auto thread = ui_thread_header_pointer(&g_app.thread_header, event, point);
        if (thread.changed)
            application_request_frame();
        if (thread.handled) {
            if (event == UiButtonPointerEvent::press) {
                if (g_app.message_composer.focused) {
                    stop_local_typing(g_app.message_composer.channel_id);
                    g_app.message_composer.focused = false;
                    ++g_app.message_composer.revision;
                    reset_message_composer_caret_timing(false);
                }
                ui_button_keyboard_cancel(&g_app.button_keyboard);
                g_app.context_focus = &g_app.shell_scene.widgets;
                g_app.button_pointer_owner = &g_app.shell_scene.widgets;
                SetFocus(g_app.window);
                SetCapture(g_app.window);
                if (GetCapture() != g_app.window) {
                    ui_thread_header_pointer(&g_app.thread_header, UiButtonPointerEvent::cancel, {});
                    g_app.button_pointer_owner = nullptr;
                }
            }
            if (event == UiButtonPointerEvent::release || cancel) {
                g_app.button_pointer_owner = nullptr;
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
            }
            if (thread.activate)
                application_thread_header_activate_target(&g_app.thread_header, thread.target);
            return !cancel;
        }
    }
    bool consumed = false;
    for (auto* owner : owners) {
        if (captured_owner && owner != captured_owner)
            continue;
        const auto* held = ui_widget_manager_resolve(*owner, owner->captured);
        if (held && held->kind != UiWidgetKind::button)
            continue;
        if (cancel && !held && owner != captured_owner)
            continue;
        const auto hit =
            cancel ? ui_widget_invalid_handle() : ui_widget_manager_hit_test(*owner, point.x, point.y);
        const auto* widget = ui_widget_manager_resolve(*owner, hit);
        if (owner == &g_app.shell_scene.widgets && widget &&
            widget->id != static_cast<u32>(UiSemantic::member_toggle) + 1 &&
            widget->id != static_cast<u32>(UiSemantic::settings) + 1 &&
            widget->id != static_cast<u32>(UiSemantic::friends_button) + 1 &&
            widget->id != static_cast<u32>(UiSemantic::guild_menu) + 1 &&
            widget->id != static_cast<u32>(UiSemantic::private_create) + 1 &&
            widget->id != static_cast<u32>(UiSemantic::private_add) + 1 &&
            widget->id != static_cast<u32>(UiSemantic::private_call) + 1 &&
            widget->id != static_cast<u32>(UiSemantic::channel_footer) + 1) {
            if (!held)
                continue;
        }
        const auto result = ui_button_pointer_target(owner, event, hit);
        if (result.changed)
            application_request_frame();
        // Hold the microphone to record; releasing anywhere else cancels.
        if (owner == &g_app.message_composer_scene.widgets)
            application_voice_message_pointer(event,
                                              ui_widget_handle_equal(hit, g_app.message_composer_scene.send),
                                              result.activate);
        if ((event == UiButtonPointerEvent::release || cancel) && owner == captured_owner) {
            g_app.button_pointer_owner = nullptr;
            if (GetCapture() == g_app.window)
                ReleaseCapture();
            consumed = true;
        }
        if (!result.handled)
            continue;
        consumed = true;
        if (event == UiButtonPointerEvent::press) {
            ui_button_keyboard_cancel(&g_app.button_keyboard);
            g_app.context_focus = owner;
            g_app.button_pointer_owner = owner;
            SetFocus(g_app.window);
            SetCapture(g_app.window);
            if (GetCapture() != g_app.window) {
                ui_button_pointer(owner, UiButtonPointerEvent::cancel, {});
                g_app.button_pointer_owner = nullptr;
            }
        }
        if (result.activate) {
            if (owner == &g_app.message_composer_scene.widgets)
                handle_composer_click(lparam);
            else
                handle_shell_click(lparam);
        }
    }
    return settings_pointer || (!cancel && consumed);
}
} // namespace maicord::app_detail
