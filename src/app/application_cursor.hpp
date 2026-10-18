#pragma once

#include "maicord/core/base.hpp"
#include "application_state.hpp"
#include "maicord/core/win32_records.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/cursor.hpp"

namespace maicord::app_detail {

// Count of applied cursors. The window procedure compares it around each
// WM_SETCURSOR so a layer that consumes the message without choosing a
// shape can never leave the previous cursor stuck on screen.
inline u32 g_application_cursor_serial = 0;
// Set by input that can change what lies under a stationary pointer; the
// next presented frame re-resolves the cursor without waiting for motion.
inline bool g_application_cursor_refresh = false;

inline const wchar_t* application_cursor_resource(UiCursor cursor) noexcept {
    switch (cursor) {
    case UiCursor::hand:
        return IDC_HAND;
    case UiCursor::ibeam:
        return IDC_IBEAM;
    case UiCursor::resize_we:
        return IDC_SIZEWE;
    case UiCursor::resize_ns:
        return IDC_SIZENS;
    case UiCursor::move:
        return IDC_SIZEALL;
    case UiCursor::hidden:
        return nullptr;
    case UiCursor::arrow:
    default:
        return IDC_ARROW;
    }
}

// The only place a semantic UiCursor becomes a native cursor.
inline void application_set_cursor(UiCursor cursor) noexcept {
    ++g_application_cursor_serial;
    const wchar_t* resource = application_cursor_resource(cursor);
    SetCursor(resource ? LoadCursorW(nullptr, resource) : nullptr);
}

// WM_SETCURSOR carries a hit-test code, not coordinates. Resolve the client
// pointer in DIPs, for the client area only.
inline bool application_cursor_point(HWND window, u32 dpi, LPARAM lparam, UiPoint* point) noexcept {
    Win32Point cursor{};
    if (!point || LOWORD(lparam) != HTCLIENT || !GetCursorPos(cursor.native()) ||
        !ScreenToClient(window, cursor.native()))
        return false;
    if (window == g_app.window)
        cursor.y -= static_cast<LONG>(g_app.renderer.caption_height);
    const float scale = 96.f / static_cast<float>(dpi ? dpi : 96u);
    *point = {static_cast<float>(cursor.x) * scale, static_cast<float>(cursor.y) * scale};
    return true;
}

inline void application_cursor_invalidate() noexcept {
    g_application_cursor_refresh = true;
}

} // namespace maicord::app_detail
