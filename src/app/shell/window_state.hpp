#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/window/window.hpp"

namespace maicord::app_detail {

// The main window's show state, activation, keyboard focus, DPI and client
// size, kept by the sink from the events that change them (state_changed,
// activation_changed, focus_changed, scale_changed) so decisions read a flag
// instead of querying the window system.
struct ApplicationWindowState {
    bool minimized;
    bool visible;
    bool foreground;
    bool keyboard_focus;
    // Zero until first queried; then the value of the last scale_changed.
    u32 dpi;
    i32 client_width;
    i32 client_height;
};
inline ApplicationWindowState g_window_state{};

// Reads the show state and client size a position change (show, hide,
// minimize, restore, resize) left.
inline void application_window_observe_position(Window window) noexcept {
    const u32 state = window_state(window);
    g_window_state.minimized = (state & state_minimized) != 0;
    g_window_state.visible = (state & state_visible) != 0;
    const WindowMetrics client = window_metrics(window);
    g_window_state.client_width = static_cast<i32>(client.width_px);
    g_window_state.client_height = static_cast<i32>(client.height_px);
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
inline u32 application_window_dpi(Window window) noexcept {
    if (!g_window_state.dpi && window)
        g_window_state.dpi = window_metrics(window).dpi;
    return g_window_state.dpi;
}


} // namespace maicord::app_detail
