#include "application_surface_motion.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::surface_motion_test {
using namespace app_detail;
u32 failures{};
ApplicationSurfaceMotionOwner zero_default_owner{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
struct FakeHost {
    ApplicationSurfaceMotionOwner owner{};
    ApplicationSurfaceMotionKey keys[kApplicationSurfaceMotionCount]{};
    float opacity[kApplicationSurfaceMotionCount]{1, 1, 1, 1, 1, 1, 1};
    bool motion_allowed{true}, gallery_covered{};
    void synchronize(u64 now) noexcept {
        for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i)
            opacity[i] =
                application_surface_motion_update(&owner, static_cast<RendererLayerSurface>(i), keys[i],
                                                  now, motion_allowed && !(i == 0 && gallery_covered));
    }
    u64 next_wake(u64 now) const noexcept {
        if (!motion_allowed)
            return 0;
        u64 wake = 0;
        for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i) {
            if (i == 0 && gallery_covered)
                continue;
            const auto deadline = application_surface_motion_owner_next_wake(
                owner, static_cast<RendererLayerSurface>(i), keys[i], now);
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
    check(forum.opacity[forum_index] == 0 && forum.next_wake(1000),
          "forum form begins a bounded entrance transition");
    forum.synchronize(1120);
    check(forum.opacity[forum_index] == 1 && !forum.next_wake(1120),
          "settled forum form has full opacity and no permanent animation wake");
    forum.keys[forum_index].lifetime = 12;
    forum.synchronize(1121);
    check(forum.opacity[forum_index] == 0 && forum.owner.slots[forum_index].motion.started_ms == 1121,
          "forum form reopening replays entrance even when the address and host draft are reused");
    forum.motion_allowed = false;
    forum.synchronize(1122);
    check(forum.opacity[forum_index] == 1 && !forum.next_wake(1122),
          "reduced motion keeps forum form fully visible without polling");
    const auto gallery = RendererLayerSurface::gallery;
    const auto popup = RendererLayerSurface::popup;
    const auto choice = RendererLayerSurface::settings_choice;
    bool zero_default = true;
    const auto* initial_bytes = reinterpret_cast<const u8*>(&zero_default_owner);
    for (u32 i = 0; i < sizeof(zero_default_owner); ++i)
        zero_default = zero_default && initial_bytes[i] == 0;
    check(zero_default && sizeof(zero_default_owner) == 688,
          "surface motion: whole static owner has a zero-filled default representation and bounded size");
    FakeHost host{};
    host.synchronize(0);
    check(host.opacity[0] == 1 && host.opacity[1] == 1 && host.opacity[2] == 1 && !host.next_wake(0),
          "surface motion: dormant surfaces stay opaque with no scheduling or bound lifetimes");
    host.keys[0] = {100, 9, 0, 0};
    host.synchronize(100);
    const auto scope = host.owner.slots[0].motion.scope;
    check(host.opacity[0] == 0 && host.next_wake(100) == 116 && host.owner.slots[0].motion.active &&
              host.owner.scope_serial == 1,
          "surface motion: fresh gallery binds one explicit scope and a 120ms entrance deadline");
    check(host.next_wake(115) == 116 && host.next_wake(116) == 116 && host.next_wake(130) == 116,
          "surface motion: observing the host deadline never moves a pending or overdue paint wake");
    host.synchronize(160);
    check(host.opacity[0] == .875f && host.owner.slots[0].motion.started_ms == 100 &&
              host.owner.slots[0].motion.scope.generation == scope.generation,
          "surface motion: hover caret reflow and publication sampling do not replace the stable scope");
    host.synchronize(160);
    check(host.opacity[0] == .875f && host.owner.scope_serial == 1 && host.next_wake(160) == 176,
          "surface motion: repeated synchronization at the same timestamp cannot restart the entrance");
    host.gallery_covered = true;
    check(!host.next_wake(165), "surface motion: covered gallery stops contributing a frame deadline");
    host.synchronize(165);
    host.gallery_covered = false;
    host.synchronize(170);
    check(host.opacity[0] == 1 && !host.next_wake(170) && host.owner.scope_serial == 1,
          "surface motion: uncovering retains identity instead of replaying a hidden entrance");

    application_surface_motion_owner_retire(&host.owner, gallery);
    host.synchronize(200);
    const auto retired_scope = host.owner.slots[0].motion.scope;
    check(host.opacity[0] == 0 && retired_scope.generation != scope.generation,
          "surface motion: explicit close and same-address reopen between frames starts a fresh scope");
    host.keys[0].owner = 10;
    host.synchronize(205);
    check(host.opacity[0] == 1 && !host.next_wake(205) &&
              !ui_motion_transition_tick(&host.owner.slots[0].motion, retired_scope, 0, 900,
                                         kUiDefaultMotionStyle, false, true),
          "surface motion: account successor settles immediately and rejects delayed old-scope ticks");
    const auto successor_scope = host.owner.slots[0].motion.scope;
    check(application_surface_motion_update(&host.owner, gallery, {100, 9, 0, 0}, 900, true) == 1 &&
              host.owner.slots[0].key.owner == 10 &&
              host.owner.slots[0].motion.scope.generation == successor_scope.generation &&
              !application_surface_motion_owner_next_wake(host.owner, gallery, {100, 9, 0, 0}, 900),
          "surface motion: stale account observation cannot replace or schedule the current owner");
    host.keys[0].scene = 101;
    host.synchronize(210);
    check(host.opacity[0] == 1 && !host.next_wake(210),
          "surface motion: unannounced gallery source-address replacement does not inherit a fade");

    host.keys[1] = {200, 70, 4, 2};
    host.synchronize(300);
    host.synchronize(360);
    const auto popup_scope = host.owner.slots[1].motion.scope;
    check(host.opacity[1] == .875f && host.opacity[0] == 1 && host.opacity[2] == 1,
          "surface motion: generic popup entrance changes only its own draw factor");
    host.keys[1].lifetime = 5;
    host.synchronize(361);
    check(host.opacity[1] == 0 && host.owner.slots[1].motion.started_ms == 361 &&
              host.owner.slots[1].motion.scope.generation != popup_scope.generation,
          "surface motion: opening counter detects popup reopen without inheriting its predecessor");
    check(application_surface_motion_update(&host.owner, popup, {200, 70, 4, 2}, 900, true) == 0 &&
              host.owner.slots[1].key.lifetime == 5 &&
              !application_surface_motion_owner_next_wake(host.owner, popup, {200, 70, 4, 2}, 900),
          "surface motion: late older popup lifetime cannot retarget or wake a newer opening");
    host.keys[1] = {};
    host.keys[2] = {200, 71, 6, 8};
    host.synchronize(400);
    check(host.opacity[1] == 1 && !host.owner.slots[1].motion.scope.owner && host.opacity[2] == 0 &&
              host.next_wake(400) == 416,
          "surface motion: choice popup uses its own slot while page and dismissed generic popup stay "
          "opaque");
    host.motion_allowed = false;
    check(!host.next_wake(410),
          "surface motion: host reduced hidden minimized or sizing policy contributes no wake");
    host.synchronize(410);
    host.motion_allowed = true;
    host.synchronize(411);
    check(host.opacity[2] == 1 && !host.next_wake(411),
          "surface motion: suppressed lifetime settles identity and does not replay after policy recovery");

    application_surface_motion_owner_retire(&host.owner, choice);
    UiMotionStyle source_style{200, UiMotionCurve::linear};
    host.owner.slots[2].style = source_style;
    source_style = {0, UiMotionCurve::ease_out};
    host.synchronize(500);
    host.synchronize(550);
    check(host.opacity[2] == .25f && host.owner.slots[2].style.duration_ms == 200 &&
              host.owner.slots[1].style.duration_ms == 120,
          "surface motion: copied per-surface style has no shared reference or effect on other slots");
    host.owner.slots[2].style = {120, UiMotionCurve::ease_out};
    host.synchronize(550);
    check(host.opacity[2] == .25f && host.owner.slots[2].motion.from == .25f &&
              host.owner.slots[2].motion.started_ms == 550,
          "surface motion: host style retarget samples continuously without geometry or scope replacement");
    host.synchronize(670);
    check(host.opacity[2] == 1 && !host.next_wake(670),
          "surface motion: settled owner retires all animation polling");
    application_surface_motion_owner_retire(&host.owner, choice);
    host.owner.slots[2].style = {1'001, static_cast<UiMotionCurve>(255)};
    host.synchronize(700);
    check(host.owner.slots[2].style.duration_ms == 120 &&
              host.owner.slots[2].style.curve == UiMotionCurve::ease_out && host.next_wake(700) == 716,
          "surface motion: malformed copied style falls back as a whole to bounded shared defaults");
    host.keys[2] = {};
    host.synchronize(710);
    check(host.opacity[2] == 1 && !host.next_wake(710) && !host.owner.slots[2].motion.scope.generation,
          "surface motion: dismissal cancels immediately with no closing-bank draw or residual deadline");
    host.owner.scope_serial = ~u64{0};
    host.keys[2] = {200, 71, 7, 8};
    host.synchronize(800);
    check(host.opacity[2] == 1 && !host.next_wake(800) && host.owner.scope_serial == ~u64{0},
          "surface motion: exhausted monotonic lifetime settles rather than wrapping scope identity");
    check(application_surface_motion_update(&host.owner, static_cast<RendererLayerSurface>(255),
                                            {1, 1, 1, 1}, 900, true) == 1 &&
              application_surface_motion_update(nullptr, gallery, {1, 1, 0, 0}, 900, true) == 1,
          "surface motion: invalid surface or missing owner cannot index outside the bounded slots");
    ApplicationSurfaceMotionOwner immediate{};
    application_surface_motion_owner_initialize(&immediate);
    immediate.slots[0].style = {0, UiMotionCurve::linear};
    application_surface_motion_owner_initialize(&immediate);
    check(application_surface_motion_update(&immediate, gallery, {300, 11, 0, 0}, 1, true) == 1 &&
              immediate.slots[0].style.duration_ms == 0 &&
              !application_surface_motion_owner_next_wake(immediate, gallery, {300, 11, 0, 0}, 1),
          "surface motion: one-time lazy defaults never replace an explicit immediate copied style");
    FakeHost paced{};
    paced.keys[0] = {400, 12, 0, 0};
    paced.synchronize(1'000);
    u64 sampled_ms = 1'000;
    float previous = 0;
    bool cadence_valid = true;
    for (u32 frame = 0; frame < 8; ++frame) {
        const u64 deadline = paced.next_wake(sampled_ms);
        cadence_valid = cadence_valid && deadline > sampled_ms && deadline <= 1'120 &&
                        paced.next_wake(deadline) == deadline && paced.next_wake(deadline + 50) == deadline;
        paced.synchronize(deadline);
        cadence_valid = cadence_valid && paced.opacity[0] >= previous;
        previous = paced.opacity[0];
        sampled_ms = deadline;
    }
    check(cadence_valid && sampled_ms == 1'120 && paced.opacity[0] == 1 && !paced.next_wake(1'120),
          "surface motion: eight host-driven frame deadlines reach the exact 120ms endpoint then retire");
    FakeHost settings{};
    const auto page = RendererLayerSurface::settings_page;
    const auto editor = RendererLayerSurface::theme_editor;
    settings.keys[3] = {500, 1, 1, 0};
    settings.synchronize(2'000);
    settings.synchronize(2'060);
    const auto page_scope = settings.owner.slots[3].motion.scope;
    check(settings.opacity[3] == .875f && settings.opacity[4] == 1,
          "surface motion: Settings body entrance affects only its own layer");
    settings.synchronize(2'061);
    check(settings.owner.slots[3].motion.scope.generation == page_scope.generation &&
              settings.owner.slots[3].motion.started_ms == 2'000,
          "surface motion: unchanged page lifetime preserves entrance through value and hover changes");
    settings.keys[3].lifetime = 2;
    settings.synchronize(2'062);
    check(settings.opacity[3] == 0 && settings.owner.slots[3].motion.started_ms == 2'062,
          "surface motion: explicit page generation begins one fresh transition");
    settings.keys[3] = {};
    settings.keys[4] = {600, 1, 8, 0};
    settings.synchronize(2'063);
    check(settings.opacity[3] == 1 && settings.opacity[4] == 0,
          "surface motion: live editor replaces page fade without reusing its scope");
    settings.synchronize(2'183);
    check(settings.opacity[4] == 1 && !settings.next_wake(2'183),
          "surface motion: settled theme editor contributes no permanent transition wake");
    application_surface_motion_owner_retire(&settings.owner, editor);
    settings.keys[4].lifetime = 9;
    settings.synchronize(2'200);
    check(settings.opacity[4] == 0 &&
              !application_surface_motion_owner_next_wake(settings.owner, editor, {600, 1, 8, 0}, 2'200) &&
              application_surface_motion_update(&settings.owner, page, {}, 2'200, true) == 1,
          "surface motion: editor reopen rejects obsolete lifetime and retired page wake");
}
} // namespace maicord::surface_motion_test

extern "C" __declspec(noreturn) void WINAPI maicord_surface_motion_entry() noexcept {
    maicord::surface_motion_test::run();
    const bool failed = maicord::surface_motion_test::failures != 0;
    constexpr char passed[] = "PASS surface motion\n", failure[] = "FAIL surface motion\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? failure : passed, sizeof(passed) - 1, &written,
              nullptr);
    ExitProcess(failed ? 1 : 0);
}
