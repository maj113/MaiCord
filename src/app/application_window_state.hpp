#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {

// The main window's show state, activation, keyboard focus, DPI and client
// size, kept by the window procedure from the messages that change them
// (WM_WINDOWPOSCHANGED, WM_ACTIVATE, WM_SETFOCUS, WM_KILLFOCUS, WM_DPICHANGED)
// so decisions read a flag instead of querying the window system.
struct ApplicationWindowState {
    bool minimized;
    bool visible;
    bool foreground;
    bool keyboard_focus;
    // A TME_LEAVE request is outstanding; WM_MOUSELEAVE spends it.
    bool pointer_leave_armed;
    // Zero until first queried; then the value of the last WM_DPICHANGED.
    u32 dpi;
    LONG client_width;
    LONG client_height;
};
inline ApplicationWindowState g_window_state{};

// Reads the show state and client size a position change (show, hide,
// minimize, restore, resize) left.
inline void application_window_observe_position(HWND window) noexcept {
    g_window_state.minimized = IsIconic(window) != FALSE;
    g_window_state.visible = IsWindowVisible(window) != FALSE;
    RECT client{};
    GetClientRect(window, &client);
    g_window_state.client_width = client.right;
    g_window_state.client_height = client.bottom;
}

inline bool application_window_minimized() noexcept {
    return g_window_state.minimized;
}
inline bool application_window_visible() noexcept {
    return g_window_state.visible;
}
// Visible and not minimized: the window can show a frame.
inline bool application_window_shown() noexcept {
    return g_window_state.visible && !g_window_state.minimized;
}
// The window is the foreground window.
inline bool application_window_foreground() noexcept {
    return g_window_state.foreground;
}
// The window holds the keyboard focus.
inline bool application_window_keyboard_focus() noexcept {
    return g_window_state.keyboard_focus;
}
// Window focus as the official client sees it (its WINDOW_FOCUS state): a
// background, minimized or tray-hidden window is unfocused.
inline bool application_window_focused() noexcept {
    return g_window_state.foreground && application_window_shown();
}
// The window's DPI; zero while the system cannot tell.
inline u32 application_window_dpi(HWND window) noexcept {
    if (!g_window_state.dpi && window)
        g_window_state.dpi = GetDpiForWindow(window);
    return g_window_state.dpi;
}

// Asks the system to post WM_MOUSELEAVE when the pointer leaves the client
// area. One request per window: it stays armed until WM_MOUSELEAVE.
inline void application_arm_pointer_leave(HWND window) noexcept {
    if (g_window_state.pointer_leave_armed)
        return;
    TRACKMOUSEEVENT tracking{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE, window, 0};
    g_window_state.pointer_leave_armed = TrackMouseEvent(&tracking) != FALSE;
}

// The cursor in client pixels, resolved once for the WM_SETCURSOR being
// dispatched so every layer that asks where the pointer is reads it instead
// of querying the system again.
struct ApplicationDispatchCursor {
    HWND window;
    POINT point;
    bool valid;
};
inline ApplicationDispatchCursor g_dispatch_cursor{};

} // namespace maicord::app_detail
