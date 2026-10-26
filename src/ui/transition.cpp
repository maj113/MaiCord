#include "maicord/ui/transition.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
float normalized(float value) noexcept {
    return value >= 0 && value <= 1 ? value : 0;
}
void sample(UiOpacityTransition& transition, u64 now_ms) noexcept {
    if (!transition.active)
        return;
    const u64 elapsed = now_ms >= transition.started_ms ? now_ms - transition.started_ms : 0;
    const float progress =
        transition.duration_ms
            ? static_cast<float>(minimum<u64>(elapsed, transition.duration_ms)) / transition.duration_ms
            : 1.0f;
    transition.value = transition.from + (transition.target - transition.from) * progress;
    if (progress >= 1) {
        transition.value = transition.target;
        transition.active = false;
    }
}
} // namespace

void ui_opacity_transition_reset(UiOpacityTransition* transition, float value) noexcept {
    if (transition)
        *transition = {0, normalized(value), normalized(value), normalized(value), 0, false};
}

bool ui_opacity_transition_tick(UiOpacityTransition* transition, float requested, u64 now_ms,
                                u32 duration_ms, bool reduced_motion, bool visible) noexcept {
    if (!transition)
        return false;
    const float previous = transition->value;
    const float target = normalized(requested);
    sample(*transition, now_ms);
    if (!visible || reduced_motion || !duration_ms || now_ms > ~u64{0} - kUiTransitionFrameMs) {
        ui_opacity_transition_reset(transition, target);
    } else if (target != transition->target || duration_ms != transition->duration_ms) {
        transition->from = transition->value;
        transition->target = target;
        transition->started_ms = now_ms;
        transition->duration_ms = duration_ms;
        transition->active = transition->from != target;
    }
    return transition->value != previous;
}

u64 ui_opacity_transition_next_wake(const UiOpacityTransition& transition, u64 now_ms, bool reduced_motion,
                                    bool visible) noexcept {
    if (!transition.active || reduced_motion || !visible || now_ms > ~u64{0} - kUiTransitionFrameMs)
        return 0;
    const u64 elapsed = now_ms >= transition.started_ms ? now_ms - transition.started_ms : 0;
    if (elapsed >= transition.duration_ms)
        return now_ms;
    const u64 remaining = transition.duration_ms - elapsed;
    return now_ms + minimum<u64>(remaining, kUiTransitionFrameMs);
}

namespace {
bool motion_scope_valid(UiMotionScope scope) noexcept {
    return scope.owner && scope.generation;
}
bool motion_scope_matches(const UiMotionTransition& transition, UiMotionScope scope) noexcept {
    return motion_scope_valid(scope) && transition.scope.owner == scope.owner &&
           transition.scope.generation == scope.generation;
}
bool motion_style_equal(const UiMotionStyle& a, const UiMotionStyle& b) noexcept {
    return a.duration_ms == b.duration_ms && a.curve == b.curve;
}
// Step response of x'' = -1000 (x - 1) - 48 x' sampled over 400 ms, integrated
// at compile time; the spring settles within the sampled window.
struct SpringTable {
    float values[33]{};
};
constexpr SpringTable make_spring() noexcept {
    SpringTable table{};
    double x = 0, v = 0;
    constexpr double dt = .0001;
    for (u32 sample = 1; sample <= 32; ++sample) {
        for (u32 step = 0; step < 125; ++step) {
            v += (-1000 * (x - 1) - 48 * v) * dt;
            x += v * dt;
        }
        table.values[sample] = static_cast<float>(x > 1 ? 1 : x);
    }
    table.values[32] = 1;
    return table;
}
constexpr SpringTable spring_table = make_spring();
float eased(float progress, UiMotionCurve curve) noexcept {
    if (curve == UiMotionCurve::spring) {
        const float position = progress * 32;
        const u32 index = position >= 32 ? 31 : static_cast<u32>(position);
        const float t = position - static_cast<float>(index);
        return spring_table.values[index] + (spring_table.values[index + 1] - spring_table.values[index]) * t;
    }
    if (curve == UiMotionCurve::css_ease_out) {
        // CSS ease-out: cubic-bezier(0, 0, .58, 1). Solve x before sampling y.
        float low = 0, high = 1;
        for (u32 i = 0; i < 16; ++i) {
            const float t = (low + high) * .5f;
            const float x = 1.74f * (1 - t) * t * t + t * t * t;
            if (x < progress) low = t; else high = t;
        }
        const float t = (low + high) * .5f;
        return t * t * (3 - 2 * t);
    }
    if (curve == UiMotionCurve::ease_out) {
        const float remaining = 1 - progress;
        return 1 - remaining * remaining * remaining;
    }
    if (curve == UiMotionCurve::ease_in_out)
        return progress * progress * (3 - 2 * progress);
    return progress;
}
void motion_sample(UiMotionTransition& transition, u64 now_ms) noexcept {
    if (!transition.active)
        return;
    const u64 elapsed = now_ms - transition.started_ms;
    if (elapsed >= transition.style.duration_ms) {
        transition.value = transition.target;
        transition.active = false;
        return;
    }
    const float progress = static_cast<float>(elapsed) / transition.style.duration_ms;
    transition.value = normalized(transition.from + (transition.target - transition.from) *
                                                        eased(progress, transition.style.curve));
}
} // namespace

float ui_motion_curve_sample(UiMotionCurve curve, float progress) noexcept {
    progress = progress > 0 ? (progress < 1 ? progress : 1) : 0;
    return eased(progress, curve);
}

bool ui_motion_style_valid(const UiMotionStyle& style) noexcept {
    return style.duration_ms <= kUiMotionMaximumDurationMs &&
           static_cast<u8>(style.curve) <= static_cast<u8>(UiMotionCurve::spring);
}

void ui_motion_transition_reset(UiMotionTransition* transition, const UiMotionScope& scope_view, float value) noexcept {
    const UiMotionScope scope = scope_view; // *transition is cleared first
    if (!transition)
        return;
    reset_value(transition);
    if (motion_scope_valid(scope)) {
        transition->scope = scope;
        transition->value = transition->from = transition->target = normalized(value);
    }
}
void ui_motion_transition_cancel(UiMotionTransition* transition) noexcept {
    if (transition)
        reset_value(transition);
}

bool ui_motion_transition_tick(UiMotionTransition* transition, const UiMotionScope& scope_view, float requested,
                               u64 now_ms, const UiMotionStyle& requested_style, bool reduced_motion,
                               bool visible) noexcept {
    const UiMotionScope scope = scope_view; // *transition is written below
    if (!transition || !motion_scope_matches(*transition, scope))
        return false;
    const float previous = transition->value;
    const float target = normalized(requested);
    const UiMotionStyle style =
        ui_motion_style_valid(requested_style) ? requested_style : kUiDefaultMotionStyle;
    now_ms = maximum(now_ms, transition->sampled_ms);
    motion_sample(*transition, now_ms);
    transition->sampled_ms = now_ms;
    if (!visible || reduced_motion || !style.duration_ms || now_ms > ~u64{0} - style.duration_ms) {
        transition->value = transition->from = transition->target = target;
        transition->started_ms = now_ms;
        transition->style = style;
        transition->active = false;
    } else if (target != transition->target || !motion_style_equal(style, transition->style)) {
        transition->from = transition->value;
        transition->target = target;
        transition->started_ms = now_ms;
        transition->style = style;
        transition->active = transition->from != target;
    }
    return transition->value != previous;
}

u64 ui_motion_transition_next_wake(const UiMotionTransition& transition, UiMotionScope scope, u64,
                                   bool reduced_motion, bool visible) noexcept {
    if (!motion_scope_matches(transition, scope) || !transition.active || reduced_motion || !visible)
        return 0;
    if (transition.started_ms > ~u64{0} - transition.style.duration_ms)
        return 0;
    const u64 completion = transition.started_ms + transition.style.duration_ms;
    // Scheduler observation cannot postpone an already scheduled frame. Even
    // overdue queries retain this deadline until a host tick samples the value.
    const u64 sampled = maximum(transition.sampled_ms, transition.started_ms);
    if (sampled > ~u64{0} - kUiTransitionFrameMs)
        return completion;
    return minimum(completion, sampled + kUiTransitionFrameMs);
}

} // namespace maicord
