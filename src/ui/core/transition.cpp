#include "maicord/ui/core/transition.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/core/geometry.hpp"

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
// Step response of x'' = -tension (x - 1) - friction x' sampled over `steps` x 32 steps of 0.1 ms, integrated at
// compile time; each spring settles within its sampled window.
struct SpringTable {
    float values[33]{};
};
constexpr SpringTable make_spring(double tension, double friction, u32 steps) noexcept {
    SpringTable table{};
    double x = 0, v = 0;
    constexpr double dt = .0001;
    for (u32 sample = 1; sample <= 32; ++sample) {
        for (u32 step = 0; step < steps; ++step) {
            v += (-tension * (x - 1) - friction * v) * dt;
            x += v * dt;
        }
        table.values[sample] = static_cast<float>(x > 1 ? 1 : x);
    }
    table.values[32] = 1;
    return table;
}
constexpr SpringTable spring_table = make_spring(1000, 48, 125);      // 400 ms
constexpr SpringTable exit_spring_table = make_spring(1200, 80, 75);  // 240 ms
constexpr SpringTable quick_spring_table = make_spring(2400, 52, 50); // 160 ms
float spring_sample(const SpringTable& table, float progress) noexcept {
    const float position = progress * 32;
    const u32 index = position >= 32 ? 31 : static_cast<u32>(position);
    const float t = position - static_cast<float>(index);
    return table.values[index] + (table.values[index + 1] - table.values[index]) * t;
}
// CSS cubic-bezier(x1, y1, x2, y2) at x = progress: Newton on x(t) from t = progress (CSS ease has a slope of at
// least .75 at both ends, so four steps converge), then y(t).
float cubic_bezier(float x1, float y1, float x2, float y2, float progress) noexcept {
    float t = progress;
    for (u32 i = 0; i < 4; ++i) {
        const float u = 1 - t;
        const float x = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t;
        const float slope = 3 * u * u * x1 + 6 * u * t * (x2 - x1) + 3 * t * t * (1 - x2);
        if (!(slope > 1e-4f))
            break;
        t -= (x - progress) / slope;
        t = t > 0 ? (t < 1 ? t : 1) : 0;
    }
    const float u = 1 - t;
    return 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t;
}
float eased(float progress, UiMotionCurve curve) noexcept {
    switch (curve) {
    case UiMotionCurve::spring:
        return spring_sample(spring_table, progress);
    case UiMotionCurve::spring_exit:
        return spring_sample(exit_spring_table, progress);
    case UiMotionCurve::spring_quick:
        return spring_sample(quick_spring_table, progress);
    case UiMotionCurve::css_ease_out: {
        // CSS ease-out: cubic-bezier(0, 0, .58, 1), so x = t^2 (1.74 - .74 t) and y = t^2 (3 - 2 t). x grows as t^2
        // from 0, so Newton from t = sqrt(x) solves it in three steps.
        if (!(progress > 0))
            return 0;
        float t = __builtin_elementwise_sqrt(progress);
        for (u32 i = 0; i < 3; ++i)
            t -= (t * t * (1.74f - .74f * t) - progress) / (t * (3.48f - 2.22f * t));
        return t * t * (3 - 2 * t);
    }
    case UiMotionCurve::css_ease:
        return cubic_bezier(.25f, .1f, .25f, 1, progress);
    case UiMotionCurve::ease_out: {
        const float remaining = 1 - progress;
        return 1 - remaining * remaining * remaining;
    }
    case UiMotionCurve::ease_in_out:
        return progress * progress * (3 - 2 * progress);
    default:
        return progress;
    }
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
           static_cast<u8>(style.curve) < static_cast<u8>(UiMotionCurve::count);
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
                               u64 now_ms, const UiMotionStyle& requested_style, bool reduced_motion) noexcept {
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
    if (reduced_motion || !style.duration_ms || now_ms > ~u64{0} - style.duration_ms) {
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

u64 ui_motion_transition_next_wake(const UiMotionTransition& transition, UiMotionScope scope,
                                   bool reduced_motion) noexcept {
    if (!motion_scope_matches(transition, scope) || !transition.active || reduced_motion)
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

UiMotionEdge ui_motion_edge(const UiRect& anchor, const UiRect& surface) noexcept {
    if (surface.top >= anchor.bottom)
        return UiMotionEdge::bottom;
    if (surface.bottom <= anchor.top)
        return UiMotionEdge::top;
    if (surface.left >= anchor.right)
        return UiMotionEdge::right;
    if (surface.right <= anchor.left)
        return UiMotionEdge::left;
    return UiMotionEdge::none;
}

namespace {
constexpr UiMotionScope kHoverScope{1, 1};
}
bool ui_motion_hover_tick(UiMotionHover* hover, u64 hovered_id, u64 now_ms, const UiMotionStyle& style,
                          bool reduced_motion) noexcept {
    if (!hover)
        return false;
    if (!hover->in.scope.owner)
        ui_motion_transition_reset(&hover->in, kHoverScope);
    if (!hover->out.scope.owner)
        ui_motion_transition_reset(&hover->out, kHoverScope);
    bool changed = hovered_id != hover->in_id;
    if (changed) {
        // The control the pointer left eases out from where it was; one it comes back to resumes its fade.
        const UiMotionTransition left = hover->in;
        const u64 left_id = hover->in_id;
        if (hovered_id && hovered_id == hover->out_id)
            hover->in = hover->out;
        else
            ui_motion_transition_reset(&hover->in, kHoverScope);
        hover->out = left;
        hover->out_id = left_id;
        hover->in_id = hovered_id;
    }
    changed = ui_motion_transition_tick(&hover->in, kHoverScope, hover->in_id ? 1.f : 0.f, now_ms, style,
                                        reduced_motion) || changed;
    return ui_motion_transition_tick(&hover->out, kHoverScope, 0, now_ms, style, reduced_motion) || changed;
}
float ui_motion_hover_amount(const UiMotionHover& hover, u64 id) noexcept {
    return !id ? 0 : id == hover.in_id ? hover.in.value : id == hover.out_id ? hover.out.value : 0;
}
u64 ui_motion_hover_next_wake(const UiMotionHover& hover) noexcept {
    const u64 a = ui_motion_transition_next_wake(hover.in, kHoverScope, false);
    const u64 b = ui_motion_transition_next_wake(hover.out, kHoverScope, false);
    return !a ? b : !b ? a : minimum(a, b);
}

} // namespace maicord
