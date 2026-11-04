#pragma once

#include "maicord/core/base.hpp"

// The double-click synthesiser of the Wayland backend (Linux). Windows delivers the second press of a double click
// as WM_xBUTTONDBLCLK (CS_DBLCLKS); Wayland only sends presses, so the backend classifies them the same way: the
// same button, within the interval of the previous press and inside the double-click rectangle centred on it. After
// a double click the sequence starts again (a third press is a plain one), as on Windows. Pure state and time in
// milliseconds that wrap, so tests drive it with numbers.

namespace maicord::wayland {

struct ClickPolicy {
    u32 interval_ms;
    float width_px;   // the double-click rectangle (SM_CXDOUBLECLK): a press matches within half of it either side
    float height_px;
};

struct ClickTracker {
    bool armed;
    u8 button;
    u32 time_ms;
    float x;
    float y;
};

inline void click_tracker_reset(ClickTracker* tracker) noexcept {
    tracker->armed = false;
}

// Records a press and returns whether it is the second of a double click.
inline bool click_tracker_press(ClickTracker* tracker, u8 button, u32 time_ms, float x, float y,
                                const ClickPolicy& policy) noexcept {
    if (tracker->armed && tracker->button == button && static_cast<u32>(time_ms - tracker->time_ms) <= policy.interval_ms) {
        const float dx = x > tracker->x ? x - tracker->x : tracker->x - x;
        const float dy = y > tracker->y ? y - tracker->y : tracker->y - y;
        if (dx * 2.0f <= policy.width_px && dy * 2.0f <= policy.height_px) {
            tracker->armed = false;
            return true;
        }
    }
    tracker->armed = true;
    tracker->button = button;
    tracker->time_ms = time_ms;
    tracker->x = x;
    tracker->y = y;
    return false;
}

} // namespace maicord::wayland
