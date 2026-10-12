#pragma once
#include "maicord/ui/background_theme.hpp"

namespace maicord {
inline constexpr u64 kBackgroundAnimationIntervalMs = 42;
struct BackgroundAnimationSample {
    u64 next_frame_ms;
    float phase_seconds;
    bool running;
};
constexpr bool background_animation_allowed(const BackgroundTheme& background, bool visible, bool minimized,
                                            bool live_resize, bool reduced_motion, bool system_motion,
                                            bool covered) noexcept {
    return background_effects_active(background) && background.speed && visible && !minimized &&
           !live_resize && !reduced_motion && system_motion && !covered;
}
// A selected plugin effect runs its own clock under the same pausing rules,
// whatever the theme's built-in effect is.
constexpr bool background_animation_allowed(const BackgroundTheme& background, bool plugin_effect, bool visible,
                                            bool minimized, bool live_resize, bool reduced_motion,
                                            bool system_motion, bool covered) noexcept {
    return (plugin_effect || (background_effects_active(background) && background.speed)) && visible &&
           !minimized && !live_resize && !reduced_motion && system_motion && !covered;
}
constexpr BackgroundAnimationSample background_animation_sample(u64 next_frame_ms, float phase_seconds,
                                                                bool running, u64 now_ms,
                                                                bool allowed) noexcept {
    if (!allowed)
        return {0, phase_seconds, false};
    if (!running || !next_frame_ms)
        return {now_ms + kBackgroundAnimationIntervalMs, phase_seconds, true};
    if (now_ms < next_frame_ms)
        return {next_frame_ms, phase_seconds, true};
    const u64 previous = next_frame_ms >= kBackgroundAnimationIntervalMs
                             ? next_frame_ms - kBackgroundAnimationIntervalMs
                             : 0;
    const u64 elapsed =
        now_ms > previous ? minimum(now_ms - previous, kBackgroundAnimationIntervalMs * 2) : 0;
    return {now_ms + kBackgroundAnimationIntervalMs, phase_seconds + static_cast<float>(elapsed) * .001f,
            true};
}
} // namespace maicord
