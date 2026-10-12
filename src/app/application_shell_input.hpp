#pragma once

#include "maicord/core/base.hpp"
#include "maicord/ui/splitter.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

float client_dip_x(LPARAM lparam) noexcept;
float client_dip_y(LPARAM lparam) noexcept;
bool set_client_cursor(i32 pixel_x, i32 pixel_y) noexcept;
bool handle_pane_resize_begin(LPARAM lparam) noexcept;
bool handle_pane_resize_drag(WPARAM buttons, LPARAM lparam) noexcept;
void end_pane_resize() noexcept;
// Escape during a splitter drag restores the width it started from.
bool cancel_pane_resize() noexcept;
bool apply_pending_pane_layout() noexcept;
bool handle_shell_hover(LPARAM lparam) noexcept;
bool handle_navigation_hover(LPARAM lparam) noexcept;
bool handle_shell_click(LPARAM lparam) noexcept;
bool handle_member_hover(LPARAM lparam) noexcept;
bool handle_message_hover(LPARAM lparam) noexcept;
// Once per presented frame: a timeline that scrolled (any cause) since the
// last frame closes the hover toolbar and clears the hovered row until the
// pointer moves again.
bool application_message_hover_observe_scroll() noexcept;
// The pointer left the client area (WM_MOUSELEAVE): close the hover toolbar
// and clear the hovered row, retiring hover-only affordances such as the
// attachment Save overlay.
bool application_message_hover_leave() noexcept;
bool handle_member_click(LPARAM lparam) noexcept;

} // namespace maicord::app_detail
