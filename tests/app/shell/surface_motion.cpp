#include "shell/surface_motion.hpp"
#include "../../support/test_output.hpp"

namespace maicord::surface_motion_test {
using namespace app_detail;
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
struct FakeHost {
    ApplicationSurfaceMotionOwner owner{};
    ApplicationSurfaceMotionKey keys[kApplicationSurfaceMotionCount]{};
    float opacity[kApplicationSurfaceMotionCount]{1, 1, 1, 1, 1, 1};
    bool motion_allowed{true}, gallery_covered{};
    void synchronize(u64 now) noexcept {
        for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i)
            opacity[i] =
                application_surface_motion_update(&owner, static_cast<RendererLayerSurface>(i), keys[i],
                                                  now, motion_allowed && !(i == 0 && gallery_covered));
    }
    u64 next_wake() const noexcept {
        if (!motion_allowed)
            return 0;
        u64 wake = 0;
        for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i) {
            if (i == 0 && gallery_covered)
                continue;
            const auto deadline = application_surface_motion_owner_next_wake(
                owner, static_cast<RendererLayerSurface>(i), keys[i]);
            if (deadline && (!wake || deadline < wake))
                wake = deadline;
        }
        return wake;
    }
};
void run() noexcept {
    FakeHost forum{};
    constexpr u32 forum_index = static_cast<u32>(RendererLayerSurface::forum_form);
    forum.keys[forum_index] = {900, 3, 11, 0};
    forum.synchronize(1000);
    check(forum.opacity[forum_index] == 0 && forum.next_wake(),
          "forum form begins a bounded entrance transition");
    forum.synchronize(1120);
    check(forum.opacity[forum_index] == 1 && !forum.next_wake(),
          "settled forum form has full opacity and no permanent animation wake");
    forum.keys[forum_index].lifetime = 12;
    forum.synchronize(1121);
    check(forum.opacity[forum_index] == 0,
          "forum form reopening replays entrance even when the address and host draft are reused");
    forum.motion_allowed = false;
    forum.synchronize(1122);
    check(forum.opacity[forum_index] == 1 && !forum.next_wake(),
          "reduced motion keeps forum form fully visible without polling");
    const auto gallery = RendererLayerSurface::gallery;
    const auto popup = RendererLayerSurface::popup;
    const auto choice = RendererLayerSurface::settings_choice;
    FakeHost host{};
    host.synchronize(0);
    check(host.opacity[0] == 1 && host.opacity[1] == 1 && host.opacity[2] == 1 && !host.next_wake(),
          "surface motion: dormant surfaces stay opaque with no scheduling or bound lifetimes");
    host.keys[0] = {100, 9, 0, 0};
    host.synchronize(100);
    check(host.opacity[0] == 0 && host.next_wake() == 116,
          "surface motion: fresh gallery binds one explicit scope and a 120ms entrance deadline");
    host.synchronize(160);
    check(host.opacity[0] == .875f,
          "surface motion: hover caret reflow and publication sampling do not replace the stable scope");
    host.synchronize(160);
    check(host.opacity[0] == .875f && host.next_wake() == 176,
          "surface motion: repeated synchronization at the same timestamp cannot restart the entrance");
    host.gallery_covered = true;
    check(!host.next_wake(), "surface motion: covered gallery stops contributing a frame deadline");
    host.synchronize(165);
    host.gallery_covered = false;
    host.synchronize(170);
    check(host.opacity[0] == 1 && !host.next_wake(),
          "surface motion: uncovering retains identity instead of replaying a hidden entrance");

    application_surface_motion_owner_retire(&host.owner, gallery);
    host.synchronize(200);
    const auto retired_scope = host.owner.slots[0].motion.scope;
    check(host.opacity[0] == 0,
          "surface motion: explicit close and same-address reopen between frames starts a fresh scope");
    host.keys[0].owner = 10;
    host.synchronize(205);
    check(host.opacity[0] == 1 && !host.next_wake() &&
              !ui_motion_transition_tick(&host.owner.slots[0].motion, retired_scope, 0, 900,
                                         kUiDefaultMotionStyle, false),
          "surface motion: account successor settles immediately and rejects delayed old-scope ticks");
    check(application_surface_motion_update(&host.owner, gallery, {100, 9, 0, 0}, 900, true) == 1 &&
              !application_surface_motion_owner_next_wake(host.owner, gallery, {100, 9, 0, 0}),
          "surface motion: stale account observation cannot replace or schedule the current owner");
    host.keys[0].scene = 101;
    host.synchronize(210);
    check(host.opacity[0] == 1 && !host.next_wake(),
          "surface motion: unannounced gallery source-address replacement does not inherit a fade");

    host.keys[1] = {200, 70, 4, 2};
    host.synchronize(300);
    host.synchronize(360);
    check(host.opacity[1] == .875f && host.opacity[0] == 1 && host.opacity[2] == 1,
          "surface motion: generic popup entrance changes only its own draw factor");
    host.keys[1].lifetime = 5;
    host.synchronize(361);
    check(host.opacity[1] == 0,
          "surface motion: opening counter detects popup reopen without inheriting its predecessor");
    check(application_surface_motion_update(&host.owner, popup, {200, 70, 4, 2}, 900, true) == 0 &&
              !application_surface_motion_owner_next_wake(host.owner, popup, {200, 70, 4, 2}),
          "surface motion: late older popup lifetime cannot retarget or wake a newer opening");
    host.keys[1] = {};
    host.keys[2] = {200, 71, 6, 8};
    host.synchronize(400);
    check(host.opacity[1] == 1 && host.opacity[2] == 0 &&
              host.next_wake() == 416,
          "surface motion: choice popup uses its own slot while the dismissed generic popup stays opaque");
    host.motion_allowed = false;
    check(!host.next_wake(),
          "surface motion: host reduced hidden minimized or sizing policy contributes no wake");
    host.synchronize(410);
    host.motion_allowed = true;
    host.synchronize(411);
    check(host.opacity[2] == 1 && !host.next_wake(),
          "surface motion: suppressed lifetime settles identity and does not replay after policy recovery");

    application_surface_motion_owner_retire(&host.owner, choice);
    host.owner.slots[2].style = {200, UiMotionCurve::linear};
    host.synchronize(500);
    host.synchronize(550);
    check(host.opacity[2] == .25f, "surface motion: a per-surface style sets the pace of its own slot");
    host.owner.slots[2].style = {120, UiMotionCurve::ease_out};
    host.synchronize(550);
    check(host.opacity[2] == .25f,
          "surface motion: host style retarget samples continuously without geometry or scope replacement");
    host.synchronize(670);
    check(host.opacity[2] == 1 && !host.next_wake(),
          "surface motion: settled owner retires all animation polling");
    application_surface_motion_owner_retire(&host.owner, choice);
    host.owner.slots[2].style = {1'001, static_cast<UiMotionCurve>(255)};
    host.synchronize(700);
    check(host.next_wake() == 716,
          "surface motion: malformed copied style falls back as a whole to bounded shared defaults");
    host.keys[2] = {};
    host.synchronize(710);
    check(host.opacity[2] == 1 && !host.next_wake(),
          "surface motion: dismissal cancels immediately with no closing-bank draw or residual deadline");
    host.owner.scope_serial = ~u64{0};
    host.keys[2] = {200, 71, 7, 8};
    host.synchronize(800);
    check(host.opacity[2] == 1 && !host.next_wake(),
          "surface motion: exhausted monotonic lifetime settles rather than wrapping scope identity");
    ApplicationSurfaceMotionOwner immediate{};
    application_surface_motion_owner_initialize(&immediate);
    immediate.slots[0].style = {0, UiMotionCurve::linear};
    application_surface_motion_owner_initialize(&immediate);
    check(application_surface_motion_update(&immediate, gallery, {300, 11, 0, 0}, 1, true) == 1 &&
              !application_surface_motion_owner_next_wake(immediate, gallery, {300, 11, 0, 0}),
          "surface motion: one-time lazy defaults never replace an explicit immediate copied style");
    FakeHost paced{};
    paced.keys[0] = {400, 12, 0, 0};
    paced.synchronize(1'000);
    u64 sampled_ms = 1'000;
    float previous = 0;
    bool cadence_valid = true;
    for (u32 frame = 0; frame < 8; ++frame) {
        const u64 deadline = paced.next_wake();
        cadence_valid = cadence_valid && deadline > sampled_ms && deadline <= 1'120;
        paced.synchronize(deadline);
        cadence_valid = cadence_valid && paced.opacity[0] >= previous;
        previous = paced.opacity[0];
        sampled_ms = deadline;
    }
    check(cadence_valid && sampled_ms == 1'120 && paced.opacity[0] == 1 && !paced.next_wake(),
          "surface motion: eight host-driven frame deadlines reach the exact 120ms endpoint then retire");
    FakeHost layers{};
    const auto profile = RendererLayerSurface::profile;
    const auto editor = RendererLayerSurface::theme_editor;
    constexpr u32 profile_index = static_cast<u32>(RendererLayerSurface::profile);
    constexpr u32 editor_index = static_cast<u32>(RendererLayerSurface::theme_editor);
    layers.keys[profile_index] = {500, 1, 1, 0};
    layers.synchronize(2'000);
    layers.synchronize(2'060);
    check(layers.opacity[profile_index] == .875f && layers.opacity[editor_index] == 1,
          "surface motion: profile entrance affects only its own layer");
    layers.keys[profile_index] = {};
    layers.keys[editor_index] = {600, 1, 8, 0};
    layers.synchronize(2'063);
    check(layers.opacity[profile_index] == 1 && layers.opacity[editor_index] == 0,
          "surface motion: live editor replaces a dismissed profile fade without reusing its scope");
    layers.synchronize(2'183);
    application_surface_motion_owner_retire(&layers.owner, editor);
    layers.keys[editor_index].lifetime = 9;
    layers.synchronize(2'200);
    check(layers.opacity[editor_index] == 0 &&
              !application_surface_motion_owner_next_wake(layers.owner, editor, {600, 1, 8, 0}) &&
              application_surface_motion_update(&layers.owner, profile, {}, 2'200, true) == 1,
          "surface motion: editor reopen rejects obsolete lifetime and retired profile wake");
}
} // namespace maicord::surface_motion_test

extern "C" __declspec(noreturn) void WINAPI maicord_surface_motion_entry() noexcept {
    maicord::surface_motion_test::run();
    maicord::test_exit(maicord::surface_motion_test::failures, "surface motion");
}
