#pragma once

#include "maicord/platform/window/event.hpp"

// The frame of a window whose client area extends over the system caption: where a pixel lies, for a frame the app
// draws (the answer to hit_test). Pixels throughout, relative to the client origin.

namespace maicord {

// The caption bar in client pixels, as the frame glue of a platform that owns the non-client frame reports it to the
// system (Windows' snap layouts flyout anchors to the maximize button).
struct FrameBarRects {
    i32 bar_right, bar_bottom;  // the bar: from the client origin to here
    i32 button[3][4];           // minimize, maximize, close: left, top, right, bottom
};

// `part` is the caption element under the point (FrameHit::client below the caption bar). A restored window resizes
// from the native side and bottom borders outside the client and from a top band of `border` pixels inside it; a
// maximized one does not, and nothing outside it is part of the window.
constexpr FrameHit window_frame_hit(i32 x, i32 y, i32 client_width, i32 client_height, i32 border, bool maximized,
                                    FrameHit part) noexcept {
    const bool inside = x >= 0 && y >= 0 && x < client_width && y < client_height;
    if (!inside && maximized)
        return FrameHit::nowhere;
    if (!maximized) {
        // Corners extend one border along each edge, as on the system frame.
        const bool left = x < border, right = x >= client_width - border;
        if (y >= client_height)
            return left ? FrameHit::bottom_left : right ? FrameHit::bottom_right : FrameHit::bottom;
        const bool top = y < border, bottom = y >= client_height - border;
        if (x < 0)
            return top ? FrameHit::top_left : bottom ? FrameHit::bottom_left : FrameHit::left;
        if (x >= client_width)
            return top ? FrameHit::top_right : bottom ? FrameHit::bottom_right : FrameHit::right;
        if (y < 0 || top)
            return left ? FrameHit::top_left : right ? FrameHit::top_right : FrameHit::top;
    }
    return part;
}

} // namespace maicord
