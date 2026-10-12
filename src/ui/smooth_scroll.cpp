#include "maicord/ui/smooth_scroll.hpp"

namespace maicord {
namespace {

constexpr double kEaseInOutX1 = 0.42;
constexpr double kEaseInOutX2 = 0.58;
constexpr double kMinimumRetargetDurationUs = 10'000.0;

constexpr double absolute(double value) noexcept {
    return value < 0.0 ? -value : value;
}

bool finite_delta(double value) noexcept {
    return value == value && value > -1.0e15 && value < 1.0e15;
}

u32 segment_duration_us(double distance, double units_per_pixel) noexcept {
    const double pixels = absolute(distance) / units_per_pixel;
    if (pixels <= 120.0) {
        return 200'000;
    }
    if (pixels >= 480.0) {
        return 100'000;
    }
    return static_cast<u32>(200'000.0 - (pixels - 120.0) * (100'000.0 / 360.0) + 0.5);
}

double cubic_bezier_axis(double parameter, double first, double second) noexcept {
    const double inverse = 1.0 - parameter;
    return 3.0 * inverse * inverse * parameter * first +
           3.0 * inverse * parameter * parameter * second +
           parameter * parameter * parameter;
}

double cubic_bezier_derivative(double parameter, double first, double second) noexcept {
    const double inverse = 1.0 - parameter;
    return 3.0 * inverse * inverse * first + 6.0 * inverse * parameter * (second - first) +
           3.0 * parameter * parameter * (1.0 - second);
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

    // The wheel curve is cubic-bezier(0.42, 0, 0.58, 1). Retargets replace the
    // first Y control point to preserve the prior velocity. X is strictly
    // monotonic, so a few Newton steps are bounded and need no general
    // timing-function allocation.
    double parameter = progress;
    for (u32 iteration = 0; iteration < 5; ++iteration) {
        const double derivative = cubic_bezier_derivative(parameter, kEaseInOutX1, kEaseInOutX2);
        if (derivative <= 0.000001) {
            break;
        }
        parameter -= (cubic_bezier_axis(parameter, kEaseInOutX1, kEaseInOutX2) - progress) /
                     derivative;
        parameter = minimum(maximum(parameter, 0.0), 1.0);
    }
    const double first_y = kEaseInOutX1 * initial_slope;
    const double dx = cubic_bezier_derivative(parameter, kEaseInOutX1, kEaseInOutX2);
    const double dy = cubic_bezier_derivative(parameter, first_y, 1.0);
    return {cubic_bezier_axis(parameter, first_y, 1.0), dx > 0.000001 ? dy / dx : 0.0};
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

i64 ui_wheel_accumulate(i64* remainder, i32 wheel_delta, i64 units_per_notch) noexcept {
    if (!remainder || units_per_notch <= 0) {
        return 0;
    }
    constexpr i64 kWheelDelta = 120;
    constexpr i64 kMaximumInputMagnitude = 32'768;
    constexpr i64 kMaximumUnitsPerNotch = static_cast<i64>(~0ull >> 1u) / kMaximumInputMagnitude;
    *remainder %= kWheelDelta;
    const i64 bounded_delta = minimum<i64>(maximum<i64>(wheel_delta, -kMaximumInputMagnitude),
                                           kMaximumInputMagnitude - 1);
    const i64 units = minimum(units_per_notch, kMaximumUnitsPerNotch);
    const i64 numerator = *remainder + bounded_delta * units;
    *remainder = numerator % kWheelDelta;
    return numerator / kWheelDelta;
}

bool ui_smooth_scroll_queue(UiSmoothScroll* scroll, double delta, u64 now_us,
                            double units_per_pixel) noexcept {
    if (!scroll || !finite_delta(delta) || !finite_delta(units_per_pixel) ||
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
    if (absolute(queued) < 0.0001) {
        *scroll = UiSmoothScroll{};
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
    scroll->units_per_pixel = static_cast<float>(units_per_pixel);
    scroll->initial_slope = static_cast<float>(
        duration_us == 0 ? 0.0
                         : minimum(maximum(prior_velocity * duration_us / queued, -1'000.0), 1'000.0));
    scroll->segment_start_us = now_us;
    scroll->last_tick_us = now_us;
    scroll->duration_us = duration_us;
    return true;
}

bool ui_smooth_scroll_step(UiSmoothScroll* scroll, u64 now_us, double* delta) noexcept {
    if (!scroll || !delta) {
        return false;
    }
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
    if (progress >= 1.0 || absolute(scroll->remaining) < 0.0001) {
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
    if (scroll) {
        *scroll = UiSmoothScroll{};
    }
}

} // namespace maicord
