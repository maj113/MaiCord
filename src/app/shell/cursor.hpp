#pragma once

#include "maicord/core/base.hpp"
#include "state.hpp"
#include "maicord/core/win32_records.hpp"
#include "content_point.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/core/cursor.hpp"

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
    case UiCursor::move:
        return IDC_SIZEALL;
    case UiCursor::hidden:
        return nullptr;
    case UiCursor::arrow:
    default:
        return IDC_ARROW;
    }
}

// Stock cursors are shared system objects: load each once, and again after a
// system settings change (a new cursor scheme). UiCursor::hidden is the last
// enumerator.
inline constexpr u32 kStockCursorCount = static_cast<u32>(UiCursor::hidden) + 1;
inline HCURSOR g_stock_cursors[kStockCursorCount]{};
inline void application_cursor_handles_reset() noexcept {
    for (HCURSOR& cursor : g_stock_cursors)
        cursor = nullptr;
}

// The only place a semantic UiCursor becomes a native cursor.
inline void application_set_cursor(UiCursor cursor) noexcept {
    ++g_application_cursor_serial;
    const u32 index = static_cast<u32>(cursor) < kStockCursorCount ? static_cast<u32>(cursor) : 0;
    HCURSOR& handle = g_stock_cursors[index];
    if (!handle)
        if (const wchar_t* resource = application_cursor_resource(static_cast<UiCursor>(index)))
            handle = LoadCursorW(nullptr, resource);
    SetCursor(handle);
}

// WM_SETCURSOR carries a hit-test code, not coordinates. Resolve the client
// pointer in DIPs, for the client area only.
inline bool application_cursor_point(HWND window, u32 dpi, LPARAM lparam, UiPoint* point) noexcept {
    Win32Point cursor{};
    if (!point || LOWORD(lparam) != HTCLIENT || !application_cursor_client(window, cursor.native()))
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
