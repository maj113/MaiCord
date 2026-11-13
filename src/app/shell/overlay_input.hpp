#pragma once

#include "cursor.hpp"
#include "state.hpp"
#include "window_state.hpp"
#include "window_events.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/platform/window/window.hpp"
#include "maicord/ui/core/input_router.hpp"
#include "maicord/ui/core/widget_manager.hpp"

namespace maicord::app_detail {

// Window-message plumbing shared by the modal overlays. The bodies are
// noinline so every overlay calls one linker-folded copy instead of inlining
// its own key-state or GetCapture sequences.

// Modifier and key state as of the message being handled. Pointer messages
// carry Shift in their wParam (MK_SHIFT): read that where the message is at
// hand instead of calling these.
__declspec(noinline) inline bool application_key_down(int key) noexcept {
    return window_key_down(static_cast<Key>(key));
}
__declspec(noinline) inline bool application_shift_down() noexcept {
    return window_key_down(key::shift);
}
__declspec(noinline) inline bool application_alt_down() noexcept {
    return window_key_down(key::alt);
}
// Raw Ctrl, including the Ctrl that AltGr synthesises.
__declspec(noinline) inline bool application_control_down() noexcept {
    return window_key_down(key::ctrl);
}
// Ctrl as a command modifier: AltGr (Ctrl+Alt) types text instead.
__declspec(noinline) inline bool application_command_down() noexcept {
    return window_key_down(key::ctrl) && !window_key_down(key::alt);
}
// Alt+F4 still closes the window while an overlay owns the keyboard.
inline bool application_alt_f4(UiInputKind kind, u32 key) noexcept {
    return kind == UiInputKind::system_key_down && key == key::f4 && application_alt_down();
}
// The press or release of the key an overlay's own close or accept consumed: its release (and auto-repeat)
// keeps routing to the overlay's input handler ahead of the text services and the router. Fills `input` when
// `event` is such a key. A release counts as a repeat, as the system's previous-state bit always said.
inline bool application_held_key_event(u32 held, const WindowEvent& event, UiInputEvent* input) noexcept {
    if (!held || (event.type != EventType::key_down && event.type != EventType::key_up) ||
        (event.flags & ev_system) != 0 || event.key.key != held)
        return false;
    *input = {event.type == EventType::key_down ? UiInputKind::key_down : UiInputKind::key_up, event.key.key,
              input_key_was_down(event)};
    return true;
}

// Pointer capture and keyboard focus belong to the main window; never
// release another owner's capture.
__declspec(noinline) inline void application_release_capture() noexcept {
    window_release_capture(g_app.window);
}
// Whether the window holds the pointer capture.
__declspec(noinline) inline bool application_pointer_captured() noexcept {
    return window_pointer_captured(g_app.window);
}
// Starts a capture. The system may refuse it: where that matters, confirm
// with application_pointer_captured.
inline void application_capture_pointer() noexcept {
    window_capture_pointer(g_app.window);
}
// Captures the widget for the pointer press in progress, then the pointer
// itself when the widget manager accepted it. The result is whether the
// window holds the capture; a false result leaves the caller to revoke.
__declspec(noinline) inline bool application_capture_widget(UiWidgetManager* widgets, UiWidgetHandle handle) noexcept {
    if (ui_widget_manager_capture(widgets, handle))
        window_capture_pointer(g_app.window);
    return window_pointer_captured(g_app.window);
}
// Takes the keyboard focus unless the window procedure has already seen it
// arrive.
inline void application_focus_window() noexcept {
    if (!application_window_keyboard_focus())
        window_focus(g_app.window);
}

// Events the pointer routes of an overlay look at: every pointer move, press, release and wheel, the cursor query
// and the context menu.
inline bool application_pointer_event(const WindowEvent& event) noexcept {
    return input_pointer(event) || event.type == EventType::cursor_query || event.type == EventType::context_menu;
}

// The system's wheel setting: lines per notch, or one page per notch.
struct WheelScroll {
    u32 lines;
    bool page;
};
__declspec(noinline) inline WheelScroll application_wheel_scroll() noexcept {
    const PlatformSettings settings = platform_settings_read();
    return {settings.wheel_lines, settings.wheel_page};
}

} // namespace maicord::app_detail
