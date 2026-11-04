#pragma once

#include "state.hpp"
#include "window_state.hpp"

namespace maicord::app_detail {

// Content pixels start below the window caption (caption.hpp):
// the main window's client rows above them belong to the caption. Without a
// caption the two coordinate spaces are the same.

// A client point (the window service's coordinates) to content pixels of the main window.
inline void application_client_to_content(POINT* point) noexcept {
    point->y -= static_cast<LONG>(g_app.renderer.caption_height);
}

// Content pixels of the main window to a client point.
inline void application_content_to_client(POINT* point) noexcept {
    point->y += static_cast<LONG>(g_app.renderer.caption_height);
}

// The cursor in client pixels of the main window: the point the window procedure resolved for the cursor query being
// dispatched, else read from the system (window_pointer_position).
inline bool application_cursor_client(POINT* point) noexcept {
    float x = 0, y = 0;
    if (!window_pointer_position(g_app.window, &x, &y))
        return false;
    point->x = static_cast<LONG>(x);
    point->y = static_cast<LONG>(y);
    return true;
}

// The cursor in content pixels of the main window.
inline bool application_cursor_content(POINT* point) noexcept {
    if (!application_cursor_client(point))
        return false;
    application_client_to_content(point);
    return true;
}

} // namespace maicord::app_detail
