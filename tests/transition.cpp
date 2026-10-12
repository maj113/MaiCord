#include "maicord/ui/transition.hpp"
#include "maicord/ui/guild_rail_scene.hpp"

namespace maicord {
void test_ui_transition(void (*check)(bool, const char*) noexcept) noexcept {
    GuildRailIndicator pill{};
    const UiMotionScope pill_scope{77, 1};
    guild_rail_indicator_tick(&pill, pill_scope, false, false, true, 0, true);
    check(pill.height.value == 0 && pill.visibility.value == 1 && !guild_rail_indicator_next_wake(pill, 0),
          "guild pill: initial unread dot is 8px and idle");
    guild_rail_indicator_tick(&pill, pill_scope, false, true, true, 100, true);
    guild_rail_indicator_tick(&pill, pill_scope, false, true, true, 200, true);
    check(pill.height.value > .255f && pill.height.value < .258f &&
          guild_rail_indicator_next_wake(pill, 200) == 216,
          "guild pill: 200ms CSS ease-out halfway is 68.46 percent, not cubic ease-out");
    const float interrupted = pill.height.value;
    guild_rail_indicator_tick(&pill, pill_scope, true, true, true, 200, true);
    check(pill.height.value == interrupted && pill.height.target == 1,
          "guild pill: selecting during hover preserves sampled height");
    guild_rail_indicator_tick(&pill, pill_scope, true, true, true, 400, true);
    check(pill.height.value == 1 && !guild_rail_indicator_next_wake(pill, 400),
          "guild pill: selected state wins over hover and settles at 40px");
    // The icon morphs with the pill: a circle (50%) at rest, a rounded square
    // (30%) while hovered or selected, sampled mid-flight.
    const UiRect icon{0, 0, 40, 40};
    GuildRailIndicator shape{};
    guild_rail_indicator_tick(&shape, pill_scope, false, false, false, 0, true);
    const float rest = guild_rail_icon_radius(icon, &shape, false, false);
    guild_rail_indicator_tick(&shape, pill_scope, false, true, false, 100, true);
    guild_rail_indicator_tick(&shape, pill_scope, false, true, false, 200, true);
    const float flight = guild_rail_icon_radius(icon, &shape, false, true);
    guild_rail_indicator_tick(&shape, pill_scope, true, false, false, 400, true);
    const float selected = guild_rail_icon_radius(icon, &shape, true, false);
    const float unsampled = guild_rail_icon_radius(icon, nullptr, true, false);
    check(rest == 20 && flight < 19.9f && flight > 12.1f && selected > 11.99f && selected < 12.01f &&
              unsampled > 11.99f && unsampled < 12.01f && guild_rail_icon_radius(icon, nullptr, false, false) == 20,
          "guild icon: the selected or hovered server morphs from a circle to a 30% rounded square");
    guild_rail_indicator_tick(&pill, pill_scope, false, false, false, 500, true);
    guild_rail_indicator_tick(&pill, pill_scope, false, false, false, 600, true);
    check(pill.height.value > .314f && pill.height.value < .316f &&
          pill.visibility.value > .314f && pill.visibility.value < .316f,
          "guild pill: deselect shrinks height and fades/translates out in parallel");
    guild_rail_indicator_tick(&pill, pill_scope, false, true, false, 610, false);
    check(pill.height.value == .375f && pill.visibility.value == 1 &&
          !guild_rail_indicator_next_wake(pill, 610),
          "guild pill: reduced motion immediately reaches hover geometry with no wake");
    guild_rail_indicator_tick(&pill, {78, 2}, false, false, false, 620, true);
    check(pill.height.value == 0 && pill.visibility.value == 0 && !guild_rail_indicator_next_wake(pill, 620),
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
              !ui_motion_transition_next_wake(motion, scope, 0, false, true),
          "motion: unbound storage cannot acquire a lifetime from a delayed tick");
    ui_motion_transition_reset(&motion, scope);
    const UiMotionStyle linear{100, UiMotionCurve::linear};
    check(!ui_motion_transition_tick(&motion, scope, 1, 100, linear, false, true) && motion.active &&
              ui_motion_transition_next_wake(motion, scope, 100, false, true) == 116,
          "motion: starting a trajectory schedules paint without changing its initial value");
    check(ui_motion_transition_next_wake(motion, scope, 115, false, true) == 116 &&
              ui_motion_transition_next_wake(motion, scope, 116, false, true) == 116 &&
              ui_motion_transition_next_wake(motion, scope, 130, false, true) == 116,
          "motion: scheduler queries preserve the absolute sampled deadline even after it is due");
    check(ui_motion_transition_tick(&motion, scope, 1, 125, linear, false, true) && motion.value == .25f,
          "motion: linear host style samples the exact borrowed-clock fraction");
    check(!ui_motion_transition_tick(&motion, scope, 1, 110, linear, false, true) && motion.value == .25f &&
              ui_motion_transition_next_wake(motion, scope, 110, false, true) == 141,
          "motion: a regressing host clock cannot rewind presentation or deadlines");
    const auto retained = motion;
    check(!ui_motion_transition_tick(&motion, {17, 2}, 0, 900, linear, false, true) &&
              !ui_motion_transition_tick(&motion, {18, 3}, 0, 900, linear, false, true) &&
              motion.value == retained.value && motion.target == retained.target &&
              motion.started_ms == retained.started_ms && motion.sampled_ms == retained.sampled_ms &&
              !ui_motion_transition_next_wake(motion, {17, 2}, 125, false, true),
          "motion: stale generations and other owners cannot touch or schedule a successor");
    const UiMotionStyle ease_out{100, UiMotionCurve::ease_out};
    check(ui_motion_transition_tick(&motion, scope, 0, 150, ease_out, false, true) && motion.value == .5f &&
              motion.from == .5f && motion.started_ms == 150,
          "motion: reversal samples the old curve before copying a new style");
    check(ui_motion_transition_tick(&motion, scope, 0, 200, ease_out, false, true) &&
              motion.value == .0625f,
          "motion: cubic ease-out reaches the exact midpoint without a transcendental curve");
    ui_motion_transition_tick(&motion, scope, 0, 245, ease_out, false, true);
    check(ui_motion_transition_next_wake(motion, scope, 245, false, true) == 250 &&
              ui_motion_transition_next_wake(motion, scope, 900, false, true) == 250 &&
              ui_motion_transition_tick(&motion, scope, 0, 250, ease_out, false, true) &&
              motion.value == 0 && !motion.active &&
              !ui_motion_transition_next_wake(motion, scope, 250, false, true),
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
              !motion.active && !ui_motion_transition_next_wake(motion, scope, 360, true, true),
          "motion: enabling reduced motion retires an in-flight trajectory at its endpoint");
    ui_motion_transition_tick(&motion, scope, 1, 400, linear, false, true);
    check(!ui_motion_transition_next_wake(motion, scope, 400, false, false) &&
              ui_motion_transition_tick(&motion, scope, 1, 410, linear, false, false) &&
              motion.value == 1 && !motion.active,
          "motion: hidden surfaces request no frame and settle when the host updates visibility");
    check(ui_motion_transition_tick(&motion, scope, 0, 420, {0, UiMotionCurve::linear}, false, true) &&
              !motion.active && motion.value == 0,
          "motion: zero-duration theme snaps without a wake");
    check(ui_motion_transition_tick(&motion, scope, 1, ~u64{0} - 50, linear, false, true) &&
              motion.value == 1 && !motion.active &&
              !ui_motion_transition_next_wake(motion, scope, ~u64{0} - 50, false, true),
          "motion: an unrepresentable completion time snaps instead of wrapping a deadline");
    ui_motion_transition_reset(&motion, {17, 4}, .75f);
    check(!ui_motion_transition_tick(&motion, scope, 0, 500, linear, false, true) && motion.value == .75f &&
              motion.scope.generation == 4,
          "motion: explicit lifetime replacement prevents an old closing fade leaking into a new page");
    ui_motion_transition_cancel(&motion);
    check(!motion.scope.owner && !motion.active &&
              !ui_motion_transition_tick(&motion, {17, 4}, 1, 600, linear, false, true) &&
              !ui_motion_transition_next_wake(motion, {17, 4}, 600, false, true),
          "motion: cancellation drops scope and deadlines without retaining host pointers");
    ui_motion_transition_reset(&motion, {0, 1}, 1);
    check(!motion.scope.owner && motion.value == 0,
          "motion: invalid lifetime cannot bind even an immediate value");
}
} // namespace maicord
