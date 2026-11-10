#pragma once

#include "maicord/core/base.hpp"
#include "state.hpp"
#include "content_point.hpp"
#include "maicord/ui/core/cursor.hpp"

namespace maicord::app_detail {

// Count of applied cursors. The window procedure compares it around each
// WM_SETCURSOR so a layer that consumes the message without choosing a
// shape can never leave the previous cursor stuck on screen.
inline u32 g_application_cursor_serial = 0;
// Set by input that can change what lies under a stationary pointer; the
// next presented frame re-resolves the cursor without waiting for motion.
inline bool g_application_cursor_refresh = false;
// The only place a semantic UiCursor reaches the window.
__declspec(noinline) inline void application_set_cursor(UiCursor cursor) noexcept {
    ++g_application_cursor_serial;
    window_set_cursor(g_app.window, cursor);
}

// The pointer's content position in DIPs, read from the window service rather than from an event (for the
// caller that is not handling a pointer event); false when the pointer cannot be read.
inline bool application_pointer_dip(UiPoint* point) noexcept {
    POINT cursor{};
    if (!application_cursor_content(&cursor))
        return false;
    const float scale = 96.f / static_cast<float>(g_app.renderer.dpi ? g_app.renderer.dpi : 96u);
    *point = {static_cast<float>(cursor.x) * scale, static_cast<float>(cursor.y) * scale};
    return true;
}

inline void application_cursor_invalidate() noexcept {
    g_application_cursor_refresh = true;
}

} // namespace maicord::app_detail
