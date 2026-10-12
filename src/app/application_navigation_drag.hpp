#pragma once
#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Drag-and-drop reordering of the guild rail (servers and folders) and the
// channel list (channels and categories, with Manage Channels). A press on a
// draggable row is held until release: without movement it activates the row
// as an ordinary click would; past the threshold it becomes a captured drag
// with a drop line or ring, edge auto-scroll and Esc to cancel.

// From the navigation click path: true when the press was taken (and its
// activation deferred to release).
bool application_navigation_drag_press(LPARAM lparam) noexcept;
// Pointer, key and capture messages while a press or drag is live.
bool application_navigation_drag_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// Per frame while dragging: edge auto-scroll and the published drop visual.
void application_navigation_drag_tick(u64 now_ms) noexcept;
void application_navigation_drag_cancel() noexcept;
} // namespace maicord::app_detail
