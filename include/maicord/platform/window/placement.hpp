#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Where the main window was, for the next start: the restored rectangle (the window neither maximized nor minimized)
// and whether the window was maximized. Windows: the outer rectangle in pixels of the virtual screen at `dpi`.
// Wayland gives no position: left and top are zero, and the size is the window geometry in logical units (dpi 96).
struct WindowPlacement {
    i32 left, top, right, bottom;
    u32 dpi;
    bool maximized;
};

struct WindowRect {
    i32 left, top, right, bottom;
};

// The limits a saved placement must keep: 50% to 1000% scale, the coordinate range of window messages, and a side of at
// least 16 pixels (so the side is not zero at any DPI).
inline constexpr u32 kWindowPlacementDpiMinimum = 48, kWindowPlacementDpiMaximum = 960;
inline constexpr i32 kWindowPlacementCoordinateLimit = 32767, kWindowPlacementSideMinimum = 16;

constexpr bool window_placement_valid(const WindowPlacement& placement) noexcept {
    constexpr i32 limit = kWindowPlacementCoordinateLimit;
    return placement.dpi >= kWindowPlacementDpiMinimum && placement.dpi <= kWindowPlacementDpiMaximum &&
           placement.left >= -limit && placement.top >= -limit && placement.right <= limit &&
           placement.bottom <= limit && placement.right - placement.left >= kWindowPlacementSideMinimum &&
           placement.bottom - placement.top >= kWindowPlacementSideMinimum;
}

// The restored rectangle of a valid placement at another DPI: the same top-left corner and the same logical size.
constexpr WindowRect window_placement_at_dpi(const WindowPlacement& placement, u32 dpi) noexcept {
    const auto scale = [&](i32 extent) noexcept {
        return static_cast<i32>((static_cast<i64>(extent) * dpi + placement.dpi / 2) / placement.dpi);
    };
    return {placement.left, placement.top, placement.left + scale(placement.right - placement.left),
            placement.top + scale(placement.bottom - placement.top)};
}

// How far a restored rectangle must reach into a work area to count as on the screen, on every edge: the official
// desktop client's test.
inline constexpr i32 kWindowPlacementVisibleInset = 32;

// Where a restored rectangle goes on a monitor with work area `work`. A rectangle that overlaps the work area inset by
// kWindowPlacementVisibleInset stays where it is. Any other rectangle moves into the work area, and becomes smaller
// first when it is larger.
constexpr WindowRect window_rect_fit(const WindowRect& rect, const WindowRect& work) noexcept {
    constexpr i32 inset = kWindowPlacementVisibleInset;
    if (rect.left < work.right - inset && rect.right > work.left + inset && rect.top < work.bottom - inset &&
        rect.bottom > work.top + inset)
        return rect;
    const i32 width = minimum(rect.right - rect.left, work.right - work.left);
    const i32 height = minimum(rect.bottom - rect.top, work.bottom - work.top);
    const i32 left = clamp_value(rect.left, work.left, work.right - width);
    const i32 top = clamp_value(rect.top, work.top, work.bottom - height);
    return {left, top, left + width, top + height};
}

} // namespace maicord
