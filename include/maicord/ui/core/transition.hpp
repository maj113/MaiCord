#pragma once
#include "maicord/core/base.hpp"

namespace maicord {

struct UiRect;

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

// Easing curves. Theme documents name the first five (client_theme_document.cpp); the rest are fixed presets.
enum class UiMotionCurve : u8 {
    linear,
    // Cubic ease-out 1 - (1 - t)^3.
    ease_out,
    // Smoothstep.
    ease_in_out,
    // CSS ease-out: cubic-bezier(0, 0, .58, 1).
    css_ease_out,
    // Damped spring (mass 1, tension 1000, friction 48) clamped to the target.
    spring,
    // Overdamped spring (mass 1, tension 1200, friction 80): modals closing. Sampled over 240 ms.
    spring_exit,
    // CSS ease: cubic-bezier(.25, .1, .25, 1), the default of CSS transitions (hover colours).
    css_ease,
    // Stiff spring (mass 1, tension 2400, friction 52) clamped to the target: tooltips. Sampled over 160 ms.
    spring_quick,
    count
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

// The shared motion vocabulary: the official client's durations and curves for its generic surfaces. A host keeps
// one UiMotionTransition per moving thing (0 hidden .. 1 shown), ticks it toward its target every frame with one of
// these styles, and maps the value through a pose below. The application side (app/shell/surface_motion.hpp:
// application_motion_tick, application_motion_schedule) adds reduced motion (the user setting and the system's)
// and the frame scheduling: frames are requested only while a transition runs. Nothing here allocates or keeps
// a timer; reduced motion and hidden surfaces settle at once.
//
// Modal: opacity and scale(.9 -> 1) on the modal spring, and back on the overdamped exit spring; reduced motion keeps
// the scale at 1.
inline constexpr UiMotionStyle kUiMotionModalEnter{400, UiMotionCurve::spring};
inline constexpr UiMotionStyle kUiMotionModalExit{240, UiMotionCurve::spring_exit};
inline constexpr float kUiMotionModalScale = .9f;
// Popout, default "translate" animation: slides kUiMotionPopoutDistance DIPs toward its anchor at full opacity,
// 200 ms CSS ease-out.
inline constexpr UiMotionStyle kUiMotionPopout{200, UiMotionCurve::css_ease_out};
inline constexpr float kUiMotionPopoutDistance = 10;
// Popout "scale" animation: opacity and scale(.01 -> 1) about the anchor edge, 120 ms CSS ease-out.
inline constexpr UiMotionStyle kUiMotionPopoutScale{120, UiMotionCurve::css_ease_out};
inline constexpr float kUiMotionPopoutScaleFrom = .01f;
// Tooltip, once its show delay passed: opacity and scale(.95 -> 1) on the stiff spring, in and out; reduced motion
// fades only.
inline constexpr UiMotionStyle kUiMotionTooltip{160, UiMotionCurve::spring_quick};
inline constexpr float kUiMotionTooltipScale = .95f;
// Hover colour: a button's background and text colours ease to their hover value and back (the official button:
// "transition: background-color 170ms ease, color 170ms ease").
inline constexpr UiMotionStyle kUiMotionHover{170, UiMotionCurve::css_ease};
// A list row's plate (the official people list: "transition: background-color .2s ease").
inline constexpr UiMotionStyle kUiMotionRowHover{200, UiMotionCurve::css_ease};
// Height expansion of a collapsible section (folders, expanding rows): 200 ms CSS ease-out.
inline constexpr UiMotionStyle kUiMotionExpand{200, UiMotionCurve::css_ease_out};

// Draw-time pose of a moving surface: multiply its opacity, scale it about its origin and translate it by (x, y)
// DIPs. Poses change paint only, never layout.
struct UiMotionPose {
    float opacity{1}, scale{1}, x{}, y{};
};
// The side of its anchor a surface opens on (a popout above its button is `top`).
enum class UiMotionEdge : u8 { none, top, bottom, left, right };
constexpr UiMotionPose ui_motion_fade(float value) noexcept {
    return {value, 1, 0, 0};
}
// Fades with value while growing from `from` (scale 1 under reduced motion: pass from = 1).
constexpr UiMotionPose ui_motion_scale(float value, float from) noexcept {
    return {value, from + (1 - from) * value, 0, 0};
}
// Starts `distance` DIPs away from the anchor on `edge` (none: above, as the official popouts do) and slides to
// rest; fades with value when `fade`.
constexpr UiMotionPose ui_motion_slide(float value, UiMotionEdge edge, float distance, bool fade) noexcept {
    const float away = (1 - value) * distance;
    return {fade ? value : 1, 1, edge == UiMotionEdge::left ? -away : edge == UiMotionEdge::right ? away : 0,
            edge == UiMotionEdge::bottom ? away : edge == UiMotionEdge::left || edge == UiMotionEdge::right ? 0 : -away};
}
// The edge of `anchor` that `surface` sits on: below, above, then beside it.
UiMotionEdge ui_motion_edge(const UiRect& anchor, const UiRect& surface) noexcept;

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
    UiMotionStyle style{0, UiMotionCurve::linear};
    bool active{};
};

void ui_motion_transition_reset(UiMotionTransition*, const UiMotionScope&, float value = 0) noexcept;
void ui_motion_transition_cancel(UiMotionTransition*) noexcept;
// Invalid styles fall back as a whole; invalid normalized targets become zero.
// Interruption preserves the sampled value, but does not promise velocity
// continuity. Hidden/reduced-motion surfaces settle to the requested endpoint.
// Returns paint-value change, not whether a deadline was started or retired.
bool ui_motion_transition_tick(UiMotionTransition*, const UiMotionScope&, float target, u64 now_ms,
                               const UiMotionStyle&, bool reduced_motion) noexcept;
// Absolute deadline anchored to the last tick, never postponed by observation.
u64 ui_motion_transition_next_wake(const UiMotionTransition&, UiMotionScope, bool reduced_motion) noexcept;

static_assert(sizeof(UiMotionStyle) == 8);
static_assert(sizeof(UiMotionScope) == 16);
static_assert(sizeof(UiMotionTransition) == 56);

// Hover colour fade over a group of controls (a list's rows, a toolbar): the hovered one eases in while the one
// the pointer left eases out. Ids are host identifiers, 0 for none. Mix colours with the amount (for example
// ui_settings_color_mix(rest, hover, amount)). Holds no references; reduced motion snaps.
struct UiMotionHover {
    UiMotionTransition in, out;
    u64 in_id, out_id;
};
// True when an amount changed.
bool ui_motion_hover_tick(UiMotionHover*, u64 hovered_id, u64 now_ms, const UiMotionStyle&,
                          bool reduced_motion) noexcept;
float ui_motion_hover_amount(const UiMotionHover&, u64 id) noexcept;
u64 ui_motion_hover_next_wake(const UiMotionHover&) noexcept;

} // namespace maicord
