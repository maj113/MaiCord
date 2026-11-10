#include "maicord/ui/core/transition.hpp"
#include "maicord/ui/guild/rail_scene.hpp"

namespace maicord {
void test_ui_transition(void (*check)(bool, const char*) noexcept) noexcept {
    GuildRailIndicator pill{};
    const UiMotionScope pill_scope{77, 1};
    guild_rail_indicator_tick(&pill, pill_scope, false, false, true, 0, true);
    check(pill.height.value == 0 && pill.visibility.value == 1 && !guild_rail_indicator_next_wake(pill),
          "guild pill: initial unread dot is 8px and idle");
    guild_rail_indicator_tick(&pill, pill_scope, false, true, true, 100, true);
    guild_rail_indicator_tick(&pill, pill_scope, false, true, true, 200, true);
    check(pill.height.value > .255f && pill.height.value < .258f &&
          guild_rail_indicator_next_wake(pill) == 216,
          "guild pill: 200ms CSS ease-out halfway is 68.46 percent, not cubic ease-out");
    const float interrupted = pill.height.value;
    guild_rail_indicator_tick(&pill, pill_scope, true, true, true, 200, true);
    check(pill.height.value == interrupted && pill.height.target == 1,
          "guild pill: selecting during hover preserves sampled height");
    guild_rail_indicator_tick(&pill, pill_scope, true, true, true, 400, true);
    check(pill.height.value == 1 && !guild_rail_indicator_next_wake(pill),
          "guild pill: selected state wins over hover and settles at 40px");
    // Rail tiles are rounded squares in every state; a folder transition's
    // shrinking tile keeps the shape.
    const UiRect icon{0, 0, 40, 40};
    const UiRect revealing{0, 10, 40, 30};
    check(guild_rail_tile_radius(icon, 12) == 12 && guild_rail_tile_radius(revealing, 12) == 6 &&
              guild_rail_tile_radius({0, 0, 4, 4}, 12) == 1.2f && guild_rail_tile_radius(icon, 30) == 20,
          "guild icon: rail tiles keep the theme radius, scaled with the tile and capped at a circle");
    guild_rail_indicator_tick(&pill, pill_scope, false, false, false, 500, true);
    guild_rail_indicator_tick(&pill, pill_scope, false, false, false, 600, true);
    check(pill.height.value > .314f && pill.height.value < .316f &&
          pill.visibility.value > .314f && pill.visibility.value < .316f,
          "guild pill: deselect shrinks height and fades/translates out in parallel");
    guild_rail_indicator_tick(&pill, pill_scope, false, true, false, 610, false);
    check(pill.height.value == .375f && pill.visibility.value == 1 &&
          !guild_rail_indicator_next_wake(pill),
          "guild pill: reduced motion immediately reaches hover geometry with no wake");
    guild_rail_indicator_tick(&pill, {78, 2}, false, false, false, 620, true);
    check(pill.height.value == 0 && pill.visibility.value == 0 && !guild_rail_indicator_next_wake(pill),
          "guild pill: rematerialized identity cannot borrow another server's animation");
    UiOpacityTransition transition{};
    check(!ui_opacity_transition_next_wake(transition, 0, false, true),
          "opacity transition: settled state schedules no idle frame");
    ui_opacity_transition_tick(&transition, 1, 100, 120, false, true);
    check(transition.active && transition.value == 0 &&
              ui_opacity_transition_next_wake(transition, 100, false, true) == 116,
          "opacity transition: explicit clock starts a bounded frame deadline");
    ui_opacity_transition_tick(&transition, 0, 160, 120, false, true);
    check(transition.active && transition.value == .5f && transition.from == .5f,
          "opacity transition: reversal samples the current trajectory without jumping");
    ui_opacity_transition_tick(&transition, 0, 280, 120, false, true);
    check(!transition.active && transition.value == 0 &&
              !ui_opacity_transition_next_wake(transition, 280, false, true),
          "opacity transition: completed fade retires its wake");
    ui_opacity_transition_tick(&transition, 1, 300, 120, true, true);
    check(transition.value == 1 && !transition.active &&
              !ui_opacity_transition_next_wake(transition, 300, true, true),
          "opacity transition: reduced motion snaps without a timer");
    ui_opacity_transition_tick(&transition, 0, 400, 120, false, false);
    check(transition.value == 0 && !transition.active &&
              !ui_opacity_transition_next_wake(transition, 400, false, false),
          "opacity transition: hidden surfaces retire interpolation rather than polling");
    ui_opacity_transition_tick(&transition, 1, ~u64{0} - 8, 120, false, true);
    check(transition.value == 1 && !transition.active &&
              !ui_opacity_transition_next_wake(transition, ~u64{0} - 8, false, true),
          "opacity transition: deadline overflow settles safely without wrapping");
    ui_opacity_transition_tick(&transition, 0, 500, 0, false, true);
    check(transition.value == 0 && !transition.active,
          "opacity transition: zero duration is an immediate host style choice");

    const UiMotionScope scope{17, 3};
    UiMotionTransition motion{};
    check(!ui_motion_transition_tick(&motion, scope, 1, 0, kUiDefaultMotionStyle, false, true) &&
              !ui_motion_transition_next_wake(motion, scope, false, true),
          "motion: unbound storage cannot acquire a lifetime from a delayed tick");
    ui_motion_transition_reset(&motion, scope);
    const UiMotionStyle linear{100, UiMotionCurve::linear};
    check(!ui_motion_transition_tick(&motion, scope, 1, 100, linear, false, true) && motion.active &&
              ui_motion_transition_next_wake(motion, scope, false, true) == 116,
          "motion: starting a trajectory schedules paint without changing its initial value");
    check(ui_motion_transition_tick(&motion, scope, 1, 125, linear, false, true) && motion.value == .25f,
          "motion: linear host style samples the exact borrowed-clock fraction");
    check(!ui_motion_transition_tick(&motion, scope, 1, 110, linear, false, true) && motion.value == .25f &&
              ui_motion_transition_next_wake(motion, scope, false, true) == 141,
          "motion: a regressing host clock cannot rewind presentation or deadlines");
    const auto retained = motion;
    check(!ui_motion_transition_tick(&motion, {17, 2}, 0, 900, linear, false, true) &&
              !ui_motion_transition_tick(&motion, {18, 3}, 0, 900, linear, false, true) &&
              motion.value == retained.value && motion.target == retained.target &&
              motion.started_ms == retained.started_ms && motion.sampled_ms == retained.sampled_ms &&
              !ui_motion_transition_next_wake(motion, {17, 2}, false, true),
          "motion: stale generations and other owners cannot touch or schedule a successor");
    const UiMotionStyle ease_out{100, UiMotionCurve::ease_out};
    check(ui_motion_transition_tick(&motion, scope, 0, 150, ease_out, false, true) && motion.value == .5f &&
              motion.from == .5f && motion.started_ms == 150,
          "motion: reversal samples the old curve before copying a new style");
    check(ui_motion_transition_tick(&motion, scope, 0, 200, ease_out, false, true) &&
              motion.value == .0625f,
          "motion: cubic ease-out reaches the exact midpoint without a transcendental curve");
    ui_motion_transition_tick(&motion, scope, 0, 245, ease_out, false, true);
    check(ui_motion_transition_next_wake(motion, scope, false, true) == 250 &&
              ui_motion_transition_tick(&motion, scope, 0, 250, ease_out, false, true) &&
              motion.value == 0 && !motion.active &&
              !ui_motion_transition_next_wake(motion, scope, false, true),
          "motion: the final deadline lands on completion and settled state has no idle wake");
    UiMotionStyle themed{100, UiMotionCurve::ease_in_out};
    ui_motion_transition_tick(&motion, scope, 1, 300, themed, false, true);
    themed = {900, UiMotionCurve::linear};
    check(motion.style.duration_ms == 100 && motion.style.curve == UiMotionCurve::ease_in_out &&
              ui_motion_transition_tick(&motion, scope, 1, 325, {100, UiMotionCurve::ease_in_out}, false,
                                        true) &&
              motion.value == .15625f,
          "motion: copied theme style survives its source changing and smoothstep has exact samples");
    const float sampled = motion.value;
    check(!ui_motion_transition_tick(&motion, scope, 1, 325, linear, false, true) &&
              motion.from == sampled && motion.value == sampled && motion.started_ms == 325 &&
              motion.style.curve == UiMotionCurve::linear,
          "motion: a live style change retimes continuously rather than jumping");
    ui_motion_transition_tick(&motion, scope, 0, 340,
                              {kUiMotionMaximumDurationMs + 1, UiMotionCurve::linear}, false, true);
    check(motion.style.duration_ms == kUiDefaultMotionStyle.duration_ms &&
              motion.style.curve == kUiDefaultMotionStyle.curve &&
              !ui_motion_style_valid({120, static_cast<UiMotionCurve>(255)}) &&
              ui_motion_style_valid({0, UiMotionCurve::ease_in_out}),
          "motion: invalid duration or curve is rejected while zero duration remains a valid style");
    ui_motion_transition_tick(&motion, scope, 1, 350, {120, static_cast<UiMotionCurve>(255)}, false, true);
    check(motion.style.curve == kUiDefaultMotionStyle.curve &&
              ui_motion_transition_tick(&motion, scope, 0, 360, linear, true, true) && motion.value == 0 &&
              !motion.active && !ui_motion_transition_next_wake(motion, scope, true, true),
          "motion: enabling reduced motion retires an in-flight trajectory at its endpoint");
    ui_motion_transition_tick(&motion, scope, 1, 400, linear, false, true);
    check(!ui_motion_transition_next_wake(motion, scope, false, false) &&
              ui_motion_transition_tick(&motion, scope, 1, 410, linear, false, false) &&
              motion.value == 1 && !motion.active,
          "motion: hidden surfaces request no frame and settle when the host updates visibility");
    check(ui_motion_transition_tick(&motion, scope, 0, 420, {0, UiMotionCurve::linear}, false, true) &&
              !motion.active && motion.value == 0,
          "motion: zero-duration theme snaps without a wake");
    check(ui_motion_transition_tick(&motion, scope, 1, ~u64{0} - 50, linear, false, true) &&
              motion.value == 1 && !motion.active &&
              !ui_motion_transition_next_wake(motion, scope, false, true),
          "motion: an unrepresentable completion time snaps instead of wrapping a deadline");
    ui_motion_transition_reset(&motion, {17, 4}, .75f);
    check(!ui_motion_transition_tick(&motion, scope, 0, 500, linear, false, true) && motion.value == .75f &&
              motion.scope.generation == 4,
          "motion: explicit lifetime replacement prevents an old closing fade leaking into a new page");
    ui_motion_transition_cancel(&motion);
    check(!motion.scope.owner && !motion.active &&
              !ui_motion_transition_tick(&motion, {17, 4}, 1, 600, linear, false, true) &&
              !ui_motion_transition_next_wake(motion, {17, 4}, false, true),
          "motion: cancellation drops scope and deadlines without retaining host pointers");
    ui_motion_transition_reset(&motion, {0, 1}, 1);
    check(!motion.scope.owner && motion.value == 0,
          "motion: invalid lifetime cannot bind even an immediate value");

    // The official client's curves: CSS ease (cubic-bezier(.25, .1, .25, 1)) is .8024 at x = .5; the springs start at
    // 0, rise monotonically to their first crossing and end exactly on 1.
    const float ease = ui_motion_curve_sample(UiMotionCurve::css_ease, .5f);
    check(ease > .80f && ease < .805f && ui_motion_curve_sample(UiMotionCurve::css_ease, 0) == 0 &&
              ui_motion_curve_sample(UiMotionCurve::css_ease, 1) == 1,
          "motion: CSS ease solves its bezier");
    bool springs = true;
    constexpr UiMotionCurve kSprings[]{UiMotionCurve::spring_exit, UiMotionCurve::spring_quick};
    for (const UiMotionCurve curve : kSprings) {
        float previous = 0;
        springs = springs && ui_motion_curve_sample(curve, 0) == 0 && ui_motion_curve_sample(curve, 1) == 1;
        for (u32 step = 1; step <= 8; ++step) {
            const float value = ui_motion_curve_sample(curve, static_cast<float>(step) / 32);
            springs = springs && value > previous && value <= 1;
            previous = value;
        }
    }
    check(springs && ui_motion_curve_sample(UiMotionCurve::spring_quick, .3125f) >= .97f &&
              ui_motion_curve_sample(UiMotionCurve::spring_exit, .5f) < .9f &&
              ui_motion_style_valid(kUiMotionTooltip) && ui_motion_style_valid(kUiMotionModalExit) &&
              ui_motion_style_valid(kUiMotionHover) && !ui_motion_style_valid({120, UiMotionCurve::count}),
          "motion: the tooltip spring nears its target by 50 ms, the exit spring is overdamped");
    const UiMotionPose slide_down = ui_motion_slide(0, UiMotionEdge::bottom, 10, false);
    const UiMotionPose slide_left = ui_motion_slide(.5f, UiMotionEdge::left, 10, true);
    const UiMotionPose grown = ui_motion_scale(.5f, .9f);
    check(slide_down.y == 10 && slide_down.x == 0 && slide_down.opacity == 1 && slide_left.x == -5 &&
              slide_left.y == 0 && slide_left.opacity == .5f && ui_motion_slide(0, UiMotionEdge::top, 10, false).y == -10 &&
              grown.scale == .95f && grown.opacity == .5f,
          "motion: poses start away from the anchor and grow toward rest");
    check(ui_motion_edge({0, 0, 10, 10}, {0, 12, 40, 40}) == UiMotionEdge::bottom &&
              ui_motion_edge({0, 50, 10, 60}, {0, 0, 40, 40}) == UiMotionEdge::top &&
              ui_motion_edge({0, 0, 10, 60}, {20, 0, 40, 40}) == UiMotionEdge::right &&
              ui_motion_edge({50, 0, 60, 60}, {0, 0, 40, 40}) == UiMotionEdge::left &&
              ui_motion_edge({0, 0, 60, 60}, {10, 10, 40, 40}) == UiMotionEdge::none,
          "motion: the anchor edge a surface opens on");
    UiMotionHover hover{};
    check(ui_motion_hover_tick(&hover, 3, 1000, {100, UiMotionCurve::linear}, false) &&
              ui_motion_hover_amount(hover, 3) == 0 && ui_motion_hover_next_wake(hover) == 1016,
          "motion hover: the hovered control starts its fade");
    ui_motion_hover_tick(&hover, 4, 1050, {100, UiMotionCurve::linear}, false);
    check(ui_motion_hover_amount(hover, 3) == .5f && ui_motion_hover_amount(hover, 4) == 0 &&
              ui_motion_hover_amount(hover, 0) == 0,
          "motion hover: the control left behind eases out from where it was");
    ui_motion_hover_tick(&hover, 3, 1075, {100, UiMotionCurve::linear}, false);
    check(ui_motion_hover_amount(hover, 3) == .375f && ui_motion_hover_amount(hover, 4) == .25f,
          "motion hover: returning to a fading control resumes it");
    check(ui_motion_hover_tick(&hover, 0, 1080, {100, UiMotionCurve::linear}, true) &&
              ui_motion_hover_amount(hover, 3) == 0 && ui_motion_hover_amount(hover, 4) == 0 &&
              !ui_motion_hover_next_wake(hover),
          "motion hover: reduced motion snaps every amount without a wake");
}
} // namespace maicord
