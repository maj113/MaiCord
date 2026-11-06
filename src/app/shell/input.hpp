#pragma once

#include "maicord/core/base.hpp"
#include "state.hpp"

#include "maicord/platform/window/event.hpp"

namespace maicord::app_detail {

// The content-pixel position of a pointer, wheel or cursor event in DIPs.
inline float client_dip_x(const WindowEvent& event) noexcept {
    return event.pointer.x * (96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u)));
}
inline float client_dip_y(const WindowEvent& event) noexcept {
    return event.pointer.y * (96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u)));
}
// A cursor query's point in DIPs; false when the system could not tell where the pointer is.
inline bool client_cursor_dip(const WindowEvent& event, UiPoint* point) noexcept {
    if (!point || event.pointer.x != event.pointer.x)
        return false;
    const float scale = 96.f / static_cast<float>(g_app.renderer.dpi ? g_app.renderer.dpi : 96u);
    *point = {event.pointer.x * scale, event.pointer.y * scale};
    return true;
}
// The point under a wheel notch in DIPs (computed in double, as the screen-point conversion was).
inline UiPoint client_wheel_dip(const WindowEvent& event) noexcept {
    const double scale = 96.0 / maximum(g_app.renderer.dpi, 1u);
    return {static_cast<float>(event.wheel.x * scale), static_cast<float>(event.wheel.y * scale)};
}
// The point of a context-menu event in DIPs, converted the way the wheel's screen point was.
inline UiPoint client_context_dip(const WindowEvent& event) noexcept {
    const double scale = 96.0 / maximum(g_app.renderer.dpi, 1u);
    return {static_cast<float>(event.pointer.x * scale), static_cast<float>(event.pointer.y * scale)};
}
bool set_client_cursor(i32 pixel_x, i32 pixel_y) noexcept;
bool handle_pane_resize_begin(const WindowEvent& event) noexcept;
bool handle_pane_resize_drag(const WindowEvent& event) noexcept;
void end_pane_resize() noexcept;
// Escape during a splitter drag restores the width it started from.
bool cancel_pane_resize() noexcept;
bool apply_pending_pane_layout() noexcept;
bool handle_shell_hover(const WindowEvent& event) noexcept;
bool handle_navigation_hover(const WindowEvent& event) noexcept;
bool handle_shell_click(const WindowEvent& event) noexcept;
bool handle_member_hover(const WindowEvent& event) noexcept;
bool handle_message_hover(const WindowEvent& event) noexcept;
// Once per presented frame: a timeline that scrolled (any cause) since the
// last frame closes the hover toolbar and clears the hovered row until the
// pointer moves again.
bool application_message_hover_observe_scroll() noexcept;
// The pointer left the client area (WM_MOUSELEAVE): close the hover toolbar
// and clear the hovered row, retiring hover-only affordances such as the
// attachment Save overlay.
bool application_message_hover_leave() noexcept;
bool handle_member_click(const WindowEvent& event) noexcept;

} // namespace maicord::app_detail
