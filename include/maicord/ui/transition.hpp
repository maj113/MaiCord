#pragma once
#include "maicord/core/base.hpp"

namespace maicord {

inline constexpr u32 kUiTransitionFrameMs = 16;

// Borrowed host time, no timer or allocation. The value is always normalized.
struct UiOpacityTransition {
    u64 started_ms{};
    float value{}, from{}, target{};
    u32 duration_ms{};
    bool active{};
};

void ui_opacity_transition_reset(UiOpacityTransition*, float value = 0) noexcept;
bool ui_opacity_transition_tick(UiOpacityTransition*, float target, u64 now_ms, u32 duration_ms,
                                bool reduced_motion, bool visible) noexcept;
u64 ui_opacity_transition_next_wake(const UiOpacityTransition&, u64 now_ms, bool reduced_motion,
                                    bool visible) noexcept;

static_assert(sizeof(UiOpacityTransition) == 32);

enum class UiMotionCurve : u8 {
    linear,
    ease_out,
    ease_in_out,
    css_ease_out,
    // Damped spring (mass 1, tension 1000, friction 48) clamped to the target.
    spring
};

inline constexpr u32 kUiMotionMaximumDurationMs = 1'000;
struct UiMotionStyle {
    u32 duration_ms{120};
    UiMotionCurve curve{UiMotionCurve::ease_out};
};
inline constexpr UiMotionStyle kUiDefaultMotionStyle{};
bool ui_motion_style_valid(const UiMotionStyle&) noexcept;
// Eased value of a normalized progress in [0, 1].
float ui_motion_curve_sample(UiMotionCurve, float progress) noexcept;

// Opaque nonzero host identifiers, never borrowed pointers. Reset is the only
// operation that binds a new lifetime; stale ticks cannot retarget its successor.
struct UiMotionScope {
    u64 owner{}, generation{};
};

// A copied, normalized paint parameter. Hosts can map it to opacity, color or
// translation without reflow. No timers, allocation or references to theme data.
struct UiMotionTransition {
    UiMotionScope scope{};
    u64 started_ms{}, sampled_ms{};
    float value{}, from{}, target{};
    UiMotionStyle style{};
    bool active{};
};

void ui_motion_transition_reset(UiMotionTransition*, UiMotionScope, float value = 0) noexcept;
void ui_motion_transition_cancel(UiMotionTransition*) noexcept;
// Invalid styles fall back as a whole; invalid normalized targets become zero.
// Interruption preserves the sampled value, but does not promise velocity
// continuity. Hidden/reduced-motion surfaces settle to the requested endpoint.
// Returns paint-value change, not whether a deadline was started or retired.
bool ui_motion_transition_tick(UiMotionTransition*, UiMotionScope, float target, u64 now_ms,
                               const UiMotionStyle&, bool reduced_motion, bool visible) noexcept;
// Absolute deadline anchored to the last tick, never postponed by observation.
u64 ui_motion_transition_next_wake(const UiMotionTransition&, UiMotionScope, u64 now_ms,
                                   bool reduced_motion, bool visible) noexcept;

static_assert(sizeof(UiMotionStyle) == 8);
static_assert(sizeof(UiMotionScope) == 16);
static_assert(sizeof(UiMotionTransition) == 56);

} // namespace maicord
