#include "maicord/ui/layout/smooth_scroll.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr double kMinimumRetargetDurationUs = 10'000.0;

bool finite_delta(double value) noexcept {
    return value == value && value > -1.0e15 && value < 1.0e15;
}

u32 segment_duration_us(double distance, double units_per_pixel) noexcept {
    const double pixels = absolute_value(distance) / units_per_pixel;
    if (pixels <= 120.0) {
        return 200'000;
    }
    if (pixels >= 480.0) {
        return 100'000;
    }
    return static_cast<u32>(200'000.0 - (pixels - 120.0) * (100'000.0 / 360.0) + 0.5);
}

struct CurveSample {
    double value;
    double slope;
};

CurveSample curve_sample(double progress, double initial_slope) noexcept {
    if (progress <= 0.0) {
        return {0.0, initial_slope};
    }
    if (progress >= 1.0) {
        return {1.0, 0.0};
    }

    // The wheel curve is cubic-bezier(0.42, 0, 0.58, 1): x = ((.52t - .78)t + 1.26)t. Retargets replace the first Y
    // control point y1 = .42 initial_slope to preserve the prior velocity. dX/dt >= 0.87 and Newton from t = progress
    // stays inside [0, 1], so two steps solve X to within 4e-8 and need no general timing-function allocation.
    double parameter = progress, slope = 0;
    for (u32 iteration = 0;; ++iteration) {
        slope = (1.56 * parameter - 1.56) * parameter + 1.26;
        if (iteration == 2)
            break;
        parameter -= (((.52 * parameter - .78) * parameter + 1.26) * parameter - progress) / slope;
    }
    // y = ((3y1 - 2)t + 3 - 6y1)t + 3y1, times t.
    const double first = 1.26 * initial_slope, a = first - 2, b = 3 - 2 * first;
    return {((a * parameter + b) * parameter + first) * parameter,
            ((3 * a * parameter + 2 * b) * parameter + first) / slope};
}

double curve_velocity(const UiSmoothScroll& scroll, u64 now_us) noexcept {
    if (!scroll.active || scroll.duration_us == 0 ||
        (now_us > scroll.segment_start_us &&
         now_us - scroll.segment_start_us >= scroll.duration_us)) {
        return 0.0;
    }
    const u64 elapsed = now_us > scroll.segment_start_us ? now_us - scroll.segment_start_us : 0;
    const CurveSample sample = curve_sample(static_cast<double>(elapsed) / scroll.duration_us,
                                            scroll.initial_slope);
    return scroll.segment_distance * sample.slope / scroll.duration_us;
}

} // namespace

bool ui_smooth_scroll_queue(UiSmoothScroll* scroll, double delta, u64 now_us,
                            double units_per_pixel) noexcept {
    if (!finite_delta(delta) || !finite_delta(units_per_pixel) ||
        units_per_pixel <= 0.0) {
        return false;
    }
    if (delta == 0.0) {
        return true;
    }
    const double prior_velocity = curve_velocity(*scroll, now_us);
    if (!scroll->active) {
        scroll->remaining = 0.0;
        scroll->last_tick_us = now_us;
        scroll->active = true;
    }
    const double queued = scroll->remaining + delta;
    if (!finite_delta(queued)) {
        return false;
    }
    if (absolute_value(queued) < 0.0001) {
        reset_value<UiSmoothScroll>(scroll);
        return true;
    }
    u32 duration_us = segment_duration_us(queued, units_per_pixel);
    if ((prior_velocity > 0.0 && queued > 0.0) ||
        (prior_velocity < 0.0 && queued < 0.0)) {
        const double velocity_bound_us = queued * 2.5 / prior_velocity;
        if (velocity_bound_us < static_cast<double>(duration_us)) {
            duration_us = velocity_bound_us < kMinimumRetargetDurationUs
                              ? 0
                              : static_cast<u32>(velocity_bound_us + 0.5);
        }
    }
    scroll->remaining = queued;
    scroll->segment_distance = queued;
    scroll->segment_emitted = 0.0;
    scroll->initial_slope = static_cast<float>(
        duration_us == 0 ? 0.0
                         : minimum(maximum(prior_velocity * duration_us / queued, -1'000.0), 1'000.0));
    scroll->segment_start_us = now_us;
    scroll->last_tick_us = now_us;
    scroll->duration_us = duration_us;
    return true;
}

bool ui_smooth_scroll_step(UiSmoothScroll* scroll, u64 now_us, double* delta) noexcept {
    *delta = 0.0;
    if (!scroll->active) {
        return true;
    }
    if (now_us <= scroll->last_tick_us) {
        return true;
    }
    const u64 elapsed_us = now_us - scroll->segment_start_us;
    scroll->last_tick_us = now_us;
    const double progress = scroll->duration_us == 0 || elapsed_us >= scroll->duration_us
                                ? 1.0
                                : static_cast<double>(elapsed_us) / scroll->duration_us;
    const double emitted =
        scroll->segment_distance * curve_sample(progress, scroll->initial_slope).value;
    double step = emitted - scroll->segment_emitted;
    scroll->segment_emitted = emitted;
    scroll->remaining -= step;
    if (progress >= 1.0 || absolute_value(scroll->remaining) < 0.0001) {
        step += scroll->remaining;
        scroll->remaining = 0.0;
        scroll->segment_distance = 0.0;
        scroll->segment_emitted = 0.0;
        scroll->active = false;
    }
    *delta = step;
    return finite_delta(step);
}

void ui_smooth_scroll_cancel(UiSmoothScroll* scroll) noexcept {
    reset_value<UiSmoothScroll>(scroll);
}

} // namespace maicord
