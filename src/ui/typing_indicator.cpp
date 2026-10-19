#include "maicord/ui/typing_indicator.hpp"

namespace maicord {
namespace {
// Pose by pulse position (0 rest .. 1 peak): small and dim until .4, growing
// until .8, full from there.
float ramp(float wave, float low) noexcept {
    return wave <= .4f ? low : wave >= .8f ? 1.f : low + (1 - low) * (wave - .4f) / .4f;
}
} // namespace
u16 ui_typing_dots_phase(u64 now_ms, bool animate) noexcept {
    return animate ? static_cast<u16>((now_ms / kUiTypingDotsStepMs) % kUiTypingDotsPhases) : kUiTypingDotsRest;
}
u64 ui_typing_dots_next_wake(u64 now_ms, bool animate) noexcept {
    if (!animate)
        return 0;
    const u64 delay = kUiTypingDotsStepMs - now_ms % kUiTypingDotsStepMs;
    return now_ms > ~u64{0} - delay ? 0 : now_ms + delay;
}
void ui_typing_dot_pose(u32 index, u16 phase, float* scale, float* opacity) noexcept {
    if (phase >= kUiTypingDotsPhases) {
        *scale = *opacity = 1;
        return;
    }
    // A triangle wave over each pulse (its peak half way), later dots behind.
    const u32 lag = (index % kUiTypingDotCount) * (kUiTypingDotsStaggerMs / kUiTypingDotsStepMs);
    const u32 local = (phase + kUiTypingDotsPhases - lag % kUiTypingDotsPhases) % kUiTypingDotsPhases;
    const float x = 2.f * static_cast<float>(local) / static_cast<float>(kUiTypingDotsPhases);
    const float wave = 1.f - (x > 1 ? x - 1 : 1 - x);
    *scale = ramp(wave, .8f);
    *opacity = ramp(wave, .3f);
}
UiRect ui_typing_dots_row(UiRect row) noexcept {
    const float left = row.left + kUiTypingDotsInset;
    return {left, row.top, left + kUiTypingDotsWidth, row.bottom};
}
void ui_typing_dots_geometry(UiRect row, u16 phase, UiTypingDot output[kUiTypingDotCount]) noexcept {
    const float middle = (row.top + row.bottom) * .5f;
    for (u32 i = 0; i < kUiTypingDotCount; ++i) {
        float scale = 1, opacity = 1;
        ui_typing_dot_pose(i, phase, &scale, &opacity);
        const float cx = row.left + kUiTypingDotSize * .5f + static_cast<float>(i) * kUiTypingDotPitch;
        const float half = kUiTypingDotSize * .5f * scale;
        output[i] = {{cx - half, middle - half, cx + half, middle + half}, opacity};
    }
}
void ui_typing_dots_quads(UiRect row, Color color, u16 phase, PaintQuad output[kUiTypingDotCount]) noexcept {
    UiTypingDot dots[kUiTypingDotCount];
    ui_typing_dots_geometry(row, phase, dots);
    for (u32 i = 0; i < kUiTypingDotCount; ++i) {
        Color ink = color;
        ink.alpha *= dots[i].opacity;
        const UiRect& r = dots[i].rectangle;
        // As paint_stream_push_rounded stores it.
        const float radius = minimum(r.right - r.left, r.bottom - r.top) * .5f;
        output[i] = {r.left, r.top, r.right, r.bottom, ink, ink, radius, 0, 0, 0};
    }
}
bool ui_typing_dots_paint(PaintStream* paint, UiRect row, Color color, u16 phase) noexcept {
    if (!paint)
        return false;
    PaintQuad quads[kUiTypingDotCount];
    ui_typing_dots_quads(row, color, phase, quads);
    for (const auto& q : quads)
        if (!paint_stream_push_rounded(paint, q.left, q.top, q.right, q.bottom, q.fill, q.radius))
            return false;
    return true;
}
bool ui_typing_dots_clock_tick(UiTypingDotsClock* clock, u64 now_ms, bool visible, bool animate) noexcept {
    if (!clock)
        return false;
    animate = visible && animate;
    const u16 phase = ui_typing_dots_phase(now_ms, animate);
    const bool changed = visible && (!clock->visible || clock->phase != phase);
    clock->phase = phase;
    clock->visible = visible;
    clock->wake_ms = ui_typing_dots_next_wake(now_ms, animate);
    return changed;
}
bool ui_typing_dots_clock_due(const UiTypingDotsClock& clock, u64 now_ms) noexcept {
    return clock.visible && clock.wake_ms && now_ms >= clock.wake_ms;
}
} // namespace maicord
