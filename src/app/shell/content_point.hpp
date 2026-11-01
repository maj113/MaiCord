#pragma once

#include "state.hpp"
#include "window_state.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {

// Content pixels start below the window caption (caption.hpp):
// the main window's client rows above them belong to the caption. Without a
// caption the two coordinate spaces are the same.

// Screen point to content pixels of the main window.
inline bool application_screen_to_content(POINT* point) noexcept {
    if (!point || !ScreenToClient(g_app.window, point))
        return false;
    point->y -= static_cast<LONG>(g_app.renderer.caption_height);
    return true;
}

// The cursor in client pixels of `window`: the point the window procedure
// resolved for the WM_SETCURSOR being dispatched, else read from the system.
inline bool application_cursor_client(HWND window, POINT* point) noexcept {
    if (g_dispatch_cursor.valid && g_dispatch_cursor.window == window) {
        *point = g_dispatch_cursor.point;
        return true;
    }
    return GetCursorPos(point) && ScreenToClient(window, point);
}

// The cursor in content pixels of the main window.
inline bool application_cursor_content(POINT* point) noexcept {
    if (!application_cursor_client(g_app.window, point))
        return false;
    point->y -= static_cast<LONG>(g_app.renderer.caption_height);
    return true;
}

// Client mouse coordinates (the lParam of a WM_MOUSEFIRST..WM_MOUSELAST message) to content coordinates. Only the
// application's own routes take the result: DefWindowProc needs the client point it was sent, or the
// WM_CONTEXTMENU it builds from WM_RBUTTONUP lands a caption above the pointer.
inline LPARAM application_caption_content_point(LPARAM client) noexcept {
    const u32 caption = g_app.renderer.caption_height;
    if (!caption)
        return client;
    const int y = static_cast<short>(HIWORD(client)) - static_cast<int>(caption);
    return MAKELPARAM(LOWORD(client), static_cast<WORD>(static_cast<short>(y)));
}

// Content pixels of the main window to a screen point.
inline bool application_content_to_screen(POINT* point) noexcept {
    if (!point)
        return false;
    point->y += static_cast<LONG>(g_app.renderer.caption_height);
    return ClientToScreen(g_app.window, point) != FALSE;
}

} // namespace maicord::app_detail
