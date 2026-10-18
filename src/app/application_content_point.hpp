#pragma once

#include "application_state.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {

// Content pixels start below the window caption (application_caption.hpp):
// the main window's client rows above them belong to the caption. Without a
// caption the two coordinate spaces are the same.

// Screen point to content pixels of the main window.
inline bool application_screen_to_content(POINT* point) noexcept {
    if (!point || !ScreenToClient(g_app.window, point))
        return false;
    point->y -= static_cast<LONG>(g_app.renderer.caption_height);
    return true;
}

// Content pixels of the main window to a screen point.
inline bool application_content_to_screen(POINT* point) noexcept {
    if (!point)
        return false;
    point->y += static_cast<LONG>(g_app.renderer.caption_height);
    return ClientToScreen(g_app.window, point) != FALSE;
}

} // namespace maicord::app_detail
