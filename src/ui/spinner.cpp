#include "maicord/ui/spinner.hpp"

namespace maicord {
namespace {
UiPoint circle_point(u32 phase) noexcept {
    constexpr UiPoint quarter[]{{1, 0},
                                {.9807853f, .1950903f},
                                {.9238795f, .3826834f},
                                {.8314696f, .5555702f},
                                {.7071068f, .7071068f},
                                {.5555702f, .8314696f},
                                {.3826834f, .9238795f},
                                {.1950903f, .9807853f}};
    const auto point = quarter[phase % 8];
    switch ((phase % kUiSpinnerPhases) / 8) {
    case 1:
        return {-point.y, point.x};
    case 2:
        return {-point.x, -point.y};
    case 3:
        return {point.y, -point.x};
    default:
        return point;
    }
}
} // namespace
u8 ui_spinner_phase(u64 now_ms, bool reduced_motion) noexcept {
    return reduced_motion ? 0 : static_cast<u8>((now_ms / kUiSpinnerStepMs) % kUiSpinnerPhases);
}
u64 ui_spinner_next_wake(u64 now_ms, bool visible, bool reduced_motion) noexcept {
    if (!visible || reduced_motion)
        return 0;
    const u64 delay = kUiSpinnerStepMs - now_ms % kUiSpinnerStepMs;
    return now_ms > ~u64{0} - delay ? 0 : now_ms + delay;
}
bool ui_spinner_clock_tick(UiSpinnerClock* clock, u64 now_ms, bool visible, bool reduced_motion) noexcept {
    if (!clock)
        return false;
    const u8 phase = visible ? ui_spinner_phase(now_ms, reduced_motion) : 0;
    const bool changed = visible && (!clock->visible || clock->phase != phase);
    clock->phase = phase;
    clock->visible = visible;
    clock->wake_ms = ui_spinner_next_wake(now_ms, visible, reduced_motion);
    return changed;
}
bool ui_spinner_clock_due(const UiSpinnerClock& clock, u64 now_ms) noexcept {
    return clock.visible && clock.wake_ms && now_ms >= clock.wake_ms;
}
bool ui_spinner_paint(PaintStream* paint, UiRect bounds, Color color, u8 phase,
                      const UiSpinnerStyle& style) noexcept {
    if (!paint || !paint_coordinate_valid(bounds.left) || !paint_coordinate_valid(bounds.top) ||
        !paint_coordinate_valid(bounds.right) || !paint_coordinate_valid(bounds.bottom) ||
        !paint_coordinate_valid(style.stroke) || style.stroke <= 0 ||
        !paint_coordinate_valid(style.tail_opacity) || style.tail_opacity < 0 || style.tail_opacity > 1)
        return false;
    const float size = minimum(ui_rect_width(bounds), ui_rect_height(bounds));
    if (size < 4 * style.stroke)
        return false;
    const UiPoint center{(bounds.left + bounds.right) * .5f, (bounds.top + bounds.bottom) * .5f};
    const float radius = (size - style.stroke) * .5f;
    constexpr u32 segments = 24;
    for (u32 i = 0; i < segments; ++i) {
        const auto a = circle_point(phase + i), b = circle_point(phase + i + 1);
        Color segment = color;
        segment.alpha *=
            style.tail_opacity + (1 - style.tail_opacity) * static_cast<float>(i) / (segments - 1);
        if (!paint_stream_push_line(paint, {center.x + a.x * radius, center.y + a.y * radius},
                                    {center.x + b.x * radius, center.y + b.y * radius}, style.stroke,
                                    segment))
            return false;
    }
    return true;
}
} // namespace maicord
