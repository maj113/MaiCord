#pragma once

#include "content_point.hpp"
#include "cursor.hpp"
#include "state.hpp"
#include "window_state.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/core/input_router.hpp"
#include "maicord/ui/layout/scroll_host.hpp"
#include "maicord/ui/core/widget_manager.hpp"

namespace maicord::app_detail {

// Window-message plumbing shared by the modal overlays. The bodies are
// noinline so every overlay calls one linker-folded copy instead of inlining
// its own GetKeyState/GetCapture sequences.

// Modifier and key state as of the message being handled. Pointer messages
// carry Shift in their wParam (MK_SHIFT): read that where the message is at
// hand instead of calling these.
__declspec(noinline) inline bool application_key_down(int key) noexcept {
    return (GetKeyState(key) & 0x8000) != 0;
}
__declspec(noinline) inline bool application_shift_down() noexcept {
    return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
}
__declspec(noinline) inline bool application_alt_down() noexcept {
    return (GetKeyState(VK_MENU) & 0x8000) != 0;
}
// Raw Ctrl, including the Ctrl that AltGr synthesises.
__declspec(noinline) inline bool application_control_down() noexcept {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}
// Ctrl as a command modifier: AltGr (Ctrl+Alt) types text instead.
__declspec(noinline) inline bool application_command_down() noexcept {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
}
// Alt+F4 still closes the window while an overlay owns the keyboard.
inline bool application_alt_f4(UiInputKind kind, u32 key) noexcept {
    return kind == UiInputKind::system_key_down && key == VK_F4 && application_alt_down();
}
// WM_KEYDOWN/WM_KEYUP of the key an overlay's own close or accept consumed:
// its release (and auto-repeat) keeps routing to the overlay's input handler
// ahead of TSF and the router. Fills `event` when `message` is such a key.
inline bool application_held_key_event(u32 held, const MSG& message, UiInputEvent* event) noexcept {
    if (!held || (message.message != WM_KEYDOWN && message.message != WM_KEYUP) || message.wParam != held)
        return false;
    *event = {message.message == WM_KEYDOWN ? UiInputKind::key_down : UiInputKind::key_up,
              static_cast<u32>(message.wParam), (message.lParam & (1ll << 30)) != 0};
    return true;
}

// Pointer capture and keyboard focus belong to the main window; never
// release another owner's capture.
__declspec(noinline) inline void application_release_capture() noexcept {
    if (GetCapture() == g_app.window)
        ReleaseCapture();
}
// Whether the window holds the pointer capture.
__declspec(noinline) inline bool application_pointer_captured() noexcept {
    return GetCapture() == g_app.window;
}
// Starts a capture. The system may refuse it: where that matters, confirm
// with application_pointer_captured.
inline void application_capture_pointer() noexcept {
    SetCapture(g_app.window);
}
// Captures the widget for the pointer press in progress, then the pointer
// itself when the widget manager accepted it. The result is whether the
// window holds the capture; a false result leaves the caller to revoke.
__declspec(noinline) inline bool application_capture_widget(UiWidgetManager* widgets, UiWidgetHandle handle) noexcept {
    if (ui_widget_manager_capture(widgets, handle))
        SetCapture(g_app.window);
    return GetCapture() == g_app.window;
}
// Takes the keyboard focus unless the window procedure has already seen it
// arrive.
inline void application_focus_window() noexcept {
    if (!application_window_keyboard_focus())
        SetFocus(g_app.window);
}

// Messages the pointer routes of an overlay look at.
inline bool application_pointer_message(UINT message) noexcept {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_SETCURSOR ||
           message == WM_CONTEXTMENU;
}

// Content DIPs of the pointer itself, for the caller that is not handling a
// pointer message; false when the cursor cannot be read.
inline bool application_pointer_dip(UiPoint* point) noexcept {
    return application_cursor_point(g_app.window, g_app.renderer.dpi, HTCLIENT, point);
}
// WM_MOUSEWHEEL/WM_MOUSEHWHEEL carry screen coordinates. Content DIPs under
// the wheel; false when the screen point cannot be mapped, with the point
// then holding the unmapped screen position.
__declspec(noinline) inline bool application_wheel_point(LPARAM lparam, UiPoint* point) noexcept {
    POINT pixel{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    const bool mapped = application_screen_to_content(&pixel);
    const double scale = 96.0 / maximum(g_app.renderer.dpi, 1u);
    *point = {static_cast<float>(pixel.x * scale), static_cast<float>(pixel.y * scale)};
    return mapped;
}
// Wheel notches of a wheel message; positive scrolls up/away from the user.
inline float application_wheel_notches(WPARAM wparam) noexcept {
    return static_cast<float>(static_cast<short>(HIWORD(wparam))) / WHEEL_DELTA;
}
// The system's lines-per-notch setting, read once and re-read after
// WM_SETTINGCHANGE (the window procedure calls the invalidate below).
inline bool g_wheel_scroll_lines_valid = false;
inline u32 g_wheel_scroll_lines = 3;
inline void application_wheel_scroll_lines_invalidate() noexcept {
    g_wheel_scroll_lines_valid = false;
}
__declspec(noinline) inline u32 application_wheel_scroll_lines() noexcept {
    if (!g_wheel_scroll_lines_valid) {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        g_wheel_scroll_lines = lines;
        g_wheel_scroll_lines_valid = true;
    }
    return g_wheel_scroll_lines;
}

} // namespace maicord::app_detail
