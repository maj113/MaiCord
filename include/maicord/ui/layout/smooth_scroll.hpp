#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// A bounded scalar form of Chromium's mouse-wheel scroll curve. The caller
// owns the actual range and applies each returned delta through the pane's
// normal clamping path, so animation cannot move content outside its model.
struct UiSmoothScroll {
    double remaining;
    double segment_distance;
    double segment_emitted;
    float initial_slope;
    u64 segment_start_us;
    u64 last_tick_us;
    u32 duration_us;
    bool active;
};

static_assert(sizeof(UiSmoothScroll) == 56);

bool ui_smooth_scroll_queue(UiSmoothScroll* scroll, double delta, u64 now_us,
                            double units_per_pixel = 1.0) noexcept;
bool ui_smooth_scroll_step(UiSmoothScroll* scroll, u64 now_us, double* delta) noexcept;
void ui_smooth_scroll_cancel(UiSmoothScroll* scroll) noexcept;

} // namespace maicord
