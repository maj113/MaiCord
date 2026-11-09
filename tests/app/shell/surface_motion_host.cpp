#include "maicord/platform/window/win32.hpp"
#include "../../support/application_downloads_inactive.hpp"
#include "../../support/application_inbox_inactive.hpp"
#include "shell/state.hpp"
#include "shell/surface_motion.hpp"
#include "shell/background.hpp"
#include "shell/window_state.hpp"
#include "../../support/test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/media/picker.hpp"
#include "maicord/ui/account/profile_scene.hpp"
#include "maicord/ui/settings/scene.hpp"

namespace maicord::surface_motion_host_test {
using namespace app_detail;
u32 failures{};
bool settings_active{};
// The effects module's answers (src/app/effects/effects.hpp) the background clock asks.
bool effect_motion_allowed = true;
u64 effect_transition_wake{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
void retire_all() noexcept {
    for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i)
        application_surface_motion_retire(static_cast<RendererLayerSurface>(i));
}

// The native show state reaches the application through the window procedure's
// position messages; this fixture window has none, so it reports them itself.
void show(HWND window, int command) noexcept {
    ShowWindow(window, command);
    application_window_observe_position(win32_window(window));
}

// The profile owner's real close, run once its exit finishes.
ProfileScene* closing_profile{};
u32 profile_closes{};
void close_profile() noexcept {
    ++profile_closes;
    closing_profile->active = false;
}

// The shared scheduler (application_motion_tick): frames are asked for only while a transition runs, each frame
// consumes its deadline, and the last one leaves the loop idle.
void shared_scheduler() noexcept {
    g_app.surface_motion.scheduled_ms = 0;
    UiMotionTransition hover{};
    const UiMotionScope scope{9, 1};
    ui_motion_transition_reset(&hover, scope);
    check(!application_surface_motion_next_wake(), "shared motion: nothing animating schedules no frame");
    application_motion_tick(&hover, scope, 1, 3000, kUiMotionHover);
    g_app.frame_pending = false;
    check(hover.active && hover.value == 0 && application_surface_motion_next_wake() == 3016 &&
              !application_surface_motion_request_frame_if_due(3015, true) && !g_app.frame_pending &&
              application_surface_motion_request_frame_if_due(3016, true) && g_app.frame_pending,
          "shared motion: a started transition asks for its next frame, not an earlier one");
    u32 frames = 0;
    float previous = 0;
    bool rising = true;
    for (u64 wake = application_surface_motion_next_wake(); wake && frames < 100;
         wake = application_surface_motion_next_wake()) {
        application_surface_motion_begin_frame(wake);
        const float value = application_motion_tick(&hover, scope, 1, wake, kUiMotionHover);
        rising = rising && value >= previous;
        previous = value;
        ++frames;
    }
    g_app.frame_pending = false;
    // 170 ms: frames every 16 ms from 3016 to 3160, then the completion at 3170.
    check(frames == 11 && rising && hover.value == 1 && !hover.active && !application_surface_motion_next_wake() &&
              !application_surface_motion_request_frame_if_due(4000, true) && !g_app.frame_pending,
          "shared motion: the transition ends at its target and frames stop");
    application_motion_tick(&hover, scope, 1, 4000, kUiMotionHover);
    check(!application_surface_motion_next_wake(), "shared motion: ticking a settled transition stays idle");
    g_app.preferences.reduced_motion = true;
    application_motion_tick(&hover, scope, 0, 4100, kUiMotionHover);
    check(hover.value == 0 && !hover.active && !application_surface_motion_next_wake(),
          "shared motion: reduced motion settles at once without a frame");
    g_app.preferences.reduced_motion = false;
    UiMotionHover rows{};
    check(application_motion_hover_tick(&rows, 5, 4200) && ui_motion_hover_amount(rows, 5) == 0 &&
              application_surface_motion_next_wake() == 4216,
          "shared motion: a hover fade starts and schedules its frame");
    application_surface_motion_begin_frame(4300);
    application_motion_hover_tick(&rows, 6, 4300);
    const float out = ui_motion_hover_amount(rows, 5);
    check(out > 0 && out < 1 && ui_motion_hover_amount(rows, 6) == 0,
          "shared motion: the row the pointer left eases out from where it was");
    application_surface_motion_begin_frame(4500);
    application_motion_hover_tick(&rows, 6, 4500);
    check(ui_motion_hover_amount(rows, 6) == 1 && ui_motion_hover_amount(rows, 5) == 0 &&
              !application_surface_motion_next_wake(),
          "shared motion: hover fades settle and stop scheduling frames");
    // A long frame (the clock jumps 50 ms between the tick and the second synchronize) neither drops the next fade
    // step nor stalls the fade: the wake the tick armed is still due afterwards, the next frame is asked for at once,
    // and the fade settles.
    UiMotionHover slow{};
    application_motion_hover_tick(&slow, 7, 5000);
    u64 start = application_surface_motion_next_wake();
    bool kept = start == 5016;
    u32 slow_frames = 0;
    for (; start && slow_frames < 100; ++slow_frames) {
        application_surface_motion_begin_frame(start);
        application_motion_hover_tick(&slow, 7, start + 5);
        application_surface_motion_synchronize(start + 50);
        const u64 wake = application_surface_motion_next_wake();
        g_app.frame_pending = false;
        kept = kept && (!wake || (wake <= start + 50 && application_surface_motion_request_frame_if_due(start + 50, true) &&
                                  g_app.frame_pending));
        start = wake ? start + 50 : 0;
    }
    g_app.frame_pending = false;
    check(kept && slow_frames > 1 && slow_frames < 100 && ui_motion_hover_amount(slow, 7) == 1 &&
              !application_surface_motion_next_wake(),
          "shared motion: a fade whose frames run 50 ms long still gets every next frame and settles");
}

void background_host(HWND window) noexcept {
    g_app.preferences = client_preferences_defaults();
    g_app.preferences.theme = ClientTheme::custom_gradient;
    g_app.preferences.gradient.effect = BackgroundEffect::stars_and_comets;
    g_app.surface_motion.system_preference_cached = true;
    g_app.surface_motion.client_area_animation = true;
    auto& r = g_app.renderer;
    r.reduced_motion = r.live_resize = false;
    r.background_phase_seconds = 3.f;
    r.background_motion_running = true;
    r.background_next_frame_ms = 10;
    show(window, SW_HIDE);
    check(!application_background_next_wake(10000) && !r.background_motion_running &&
              !r.background_next_frame_ms && r.background_phase_seconds == 3.f,
          "background host: hidden native window retires overdue shader deadline without presentation");
    show(window, SW_SHOWNOACTIVATE);
    check(
        IsWindowVisible(window) && !IsIconic(window) && application_background_next_wake(10000) == 10000 &&
            application_background_synchronize(10000) && application_background_next_wake(10001) == 10042 &&
            application_background_next_wake(10041) == 10042 && r.background_phase_seconds == 3.f,
        "background host: visible native window arms a stable future deadline without warm phase drift");
    check(application_background_synchronize(10042) && r.background_next_frame_ms == 10084 &&
              r.background_phase_seconds > 3.041f && r.background_phase_seconds < 3.043f,
          "background host: due production sample advances one bounded shader phase");
    const float paused = r.background_phase_seconds;
    show(window, SW_HIDE);
    check(!application_background_next_wake(10043) && !r.background_motion_running &&
              !r.background_next_frame_ms && r.background_phase_seconds == paused,
          "background host: hide without synchronize clears running clock and preserves shader phase");
    show(window, SW_SHOWNOACTIVATE);
    check(application_background_next_wake(500000) == 500000 &&
              application_background_synchronize(500000) && r.background_next_frame_ms == 500042 &&
              r.background_phase_seconds == paused,
          "background host: native hidden resume rearms deadline without catching up elapsed time");
    g_app.frame_pending = false;
    check(!application_background_request_frame_if_due(500042, false) && !g_app.frame_pending &&
              application_background_request_frame_if_due(500042, true) && g_app.frame_pending,
          "background host: due clock requests one frame only when a display slot is available");
    // Production acknowledges this clock at frame entry before pane preparation
    // can reject the frame. Model that later controlled rejection by not drawing.
    g_app.frame_pending = false;
    bool acknowledged = application_background_synchronize(500042) && r.background_next_frame_ms == 500084;
    for (u32 retry = 0; retry < 4; ++retry)
        acknowledged = acknowledged && !application_background_request_frame_if_due(500042, true) &&
                       !g_app.frame_pending;
    check(acknowledged && application_background_request_frame_if_due(500084, true) && g_app.frame_pending,
          "background host: pre-preparation clock acknowledgment prevents rejected-frame overdue spin "
          "and admits the next cadence normally");
    g_app.frame_pending = false;
    bool gates = true;
    for (u32 reason = 0; reason < 11; ++reason) {
        g_app.preferences.reduced_motion = false;
        r.reduced_motion = r.live_resize = false;
        g_app.surface_motion.system_preference_cached = true;
        g_app.surface_motion.client_area_animation = true;
        g_app.login.visible = settings_active = false;
        g_app.preferences.gradient.effect = BackgroundEffect::stars;
        g_app.preferences.gradient.speed = 50;
        g_app.preferences.gradient.strength = 25;
        g_app.preferences.gradient.density = 35;
        g_app.preferences.gradient.intensity = 40;
        gates = gates && application_background_synchronize(600000 + reason * 100);
        const float phase = r.background_phase_seconds;
        switch (reason) {
        case 0:
            g_app.preferences.reduced_motion = true;
            break;
        case 1:
            r.reduced_motion = true;
            break;
        case 2:
            r.live_resize = true;
            break;
        case 3:
            g_app.surface_motion.client_area_animation = false;
            break;
        case 4:
            g_app.surface_motion.system_preference_cached = false;
            break;
        case 5:
            g_app.login.visible = true;
            break;
        case 6:
            settings_active = true;
            break;
        case 7:
            g_app.preferences.gradient.effect = BackgroundEffect::none;
            break;
        case 8:
            g_app.preferences.gradient.speed = 0;
            break;
        case 9:
            g_app.preferences.gradient.strength = 0;
            break;
        default:
            g_app.preferences.gradient.density = 0;
            break;
        }
        gates = gates && !application_background_next_wake(600001 + reason * 100) &&
                !r.background_motion_running && !r.background_next_frame_ms &&
                r.background_phase_seconds == phase;
    }
    check(gates, "background host: actual production preference resize system coverage and disabled-effect "
                 "gates retire every wake");
    // A package effect's ambient input still easing (a theme switch while the effect is idle) wakes on its own
    // cadence while the clock stays paused, and the paused clock neither runs nor catches up; settled, nothing wakes.
    effect_motion_allowed = false;
    effect_transition_wake = 650042;
    const float idle_phase = r.background_phase_seconds;
    const bool easing_wake = application_background_next_wake(650000) == 650042 &&
                             application_background_synchronize(650042) && !r.background_motion_running &&
                             r.background_phase_seconds == idle_phase;
    effect_transition_wake = 0;
    check(easing_wake && !application_background_next_wake(650043),
          "background host: an easing effect input wakes frames while the paused clock keeps its phase");
    effect_motion_allowed = true;
    g_app.preferences.gradient.density = 35;
    g_app.preferences.gradient.intensity = 0;
    const float zero_intensity_phase = r.background_phase_seconds;
    check(application_background_next_wake(700000) == 700000 &&
              application_background_synchronize(700000) &&
              application_background_next_wake(700001) == 700042 &&
              r.background_phase_seconds == zero_intensity_phase,
          "background host: valid zero-intensity custom theme keeps its gentle effect clock without "
          "catching up the retired interval");
    g_app.preferences = client_preferences_defaults();
    r.reduced_motion = r.live_resize = false;
    g_app.surface_motion.system_preference_cached = true;
    g_app.surface_motion.client_area_animation = true;
    g_app.login.visible = settings_active = false;
}
void run() noexcept {
    constexpr wchar_t name[] = L"MaiCordSurfaceMotionHostTest";
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = name;
    if (!RegisterClassW(&type)) {
        check(false, "host motion: owned test window class");
        return;
    }
    // Never put a test surface on the user's desktop or activate it. Native
    // visibility/iconic checks still run against this owned offscreen HWND.
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, name, name, WS_POPUP, -32000, -32000,
                                  80, 80, nullptr, nullptr, type.hInstance, nullptr);
    if (!window) {
        check(false, "host motion: owned offscreen HWND");
        UnregisterClassW(name, type.hInstance);
        return;
    }
    g_app.window = win32_window(window);
    g_app.renderer_ready = true;
    g_app.renderer.width = g_app.renderer.height = 640;
    g_app.renderer.dpi = 96;
    g_app.renderer.layer_opacity = {1, 1, 1, 1, 1, 1};
    g_app.surface_motion.system_preference_cached = true;
    g_app.surface_motion.client_area_animation = true;
    // Reduced motion follows the system's animation setting, as by default.
    g_app.preferences.reduced_motion_sync = true;
    for (auto& transform : g_app.renderer.layer_transform)
        transform.scale = 1;
    auto& popup = g_app.popup_scene;
    popup.kind = PopupKind::context_menu;
    popup.rectangle = {20, 20, 180, 140};
    popup.owner_id = 7;
    popup.opens = 1;
    popup.active = true;
    g_app.renderer.popup_scene = &popup;
    application_surface_motion_synchronize(10);
    check(g_app.renderer.layer_opacity.popup == 1 && !application_surface_motion_next_wake(),
          "host motion: actual hidden HWND suppresses entrance and wake");
    show(window, SW_SHOWNOACTIVATE);
    ++popup.opens;
    application_surface_motion_synchronize(100);
    check(g_app.renderer.layer_opacity.popup == 0 && application_surface_motion_next_wake() == 116,
          "host motion: actual visible HWND binds an absolute entrance deadline");
    application_surface_motion_synchronize(160);
    check(g_app.renderer.layer_opacity.popup == .875f && g_app.renderer.layer_opacity.gallery == 1 &&
              g_app.renderer.layer_opacity.settings_choice == 1,
          "host motion: production adapter samples shared easing in the exact draw factor");
    g_app.preferences.reduced_motion = true;
    check(!application_surface_motion_next_wake(), "host motion: reduced preference suppresses wake");
    application_surface_motion_synchronize(161);
    check(g_app.renderer.layer_opacity.popup == 1, "host motion: reduced preference settles immediately");
    g_app.preferences.reduced_motion = false;
    ++popup.opens;
    application_surface_motion_synchronize(200);
    g_app.renderer.live_resize = true;
    check(!application_surface_motion_next_wake(), "host motion: live sizing suppresses wake");
    application_surface_motion_synchronize(201);
    check(g_app.renderer.layer_opacity.popup == 1, "host motion: live sizing settles draw state");
    g_app.renderer.live_resize = false;
    g_app.surface_motion.client_area_animation = false;
    ++popup.opens;
    application_surface_motion_synchronize(300);
    check(g_app.renderer.layer_opacity.popup == 1 && !application_surface_motion_next_wake(),
          "host motion: cached Windows motion preference prevents a trajectory");
    g_app.surface_motion.client_area_animation = true;
    UiSettingsScene settings{};
    settings.active = true;
    settings.page_generation = 1;
    g_app.renderer.settings_scene = &settings;
    popup.active = false;
    // Settings pages have no fade of their own. Opening Settings is the modal's entrance, which fades and scales
    // the whole modal as one; switching categories swaps the page at once, natively drawn or hosted alike.
    g_app.renderer.settings_modal_progress = .4f;
    application_surface_motion_synchronize(330);
    ++settings.page_generation;
    application_surface_motion_synchronize(335);
    g_app.renderer.settings_modal_progress = 1;
    ++settings.page_generation;
    application_surface_motion_synchronize(340);
    g_app.frame_pending = false;
    check(!application_surface_motion_next_wake() && !application_surface_motion_request_frame_if_due(345, true) &&
              !g_app.frame_pending,
          "host motion: Settings page switches, mid-entrance or in a settled modal, schedule no frames");
    // A Settings choice popup fades in on its own clock.
    popup.active = true;
    popup.kind = PopupKind::settings_choice;
    ++popup.opens;
    application_surface_motion_synchronize(350);
    check(g_app.renderer.layer_opacity.settings_choice == 0 && g_app.renderer.layer_opacity.popup == 1 &&
              application_surface_motion_next_wake() == 366,
          "host motion: settings choice owns its factor and exact deadline");
    bool before_due = true;
    for (u32 query = 0; query < 4; ++query)
        before_due = before_due && !application_surface_motion_request_frame_if_due(365, true) &&
                     !g_app.frame_pending;
    check(before_due && !application_surface_motion_request_frame_if_due(366, false) &&
              !g_app.frame_pending && application_surface_motion_next_wake() == 366,
          "host motion: early or unavailable display slots cannot enqueue choice opacity ticks");
    check(application_surface_motion_request_frame_if_due(366, true) && g_app.frame_pending,
          "host motion: due choice entrance requests a pending frame without hover or account input");
    // Production samples at frame entry, before later projection can fail.
    g_app.frame_pending = false;
    application_surface_motion_synchronize(366);
    check(g_app.renderer.layer_opacity.settings_choice > 0 &&
              g_app.renderer.layer_opacity.settings_choice < 1 &&
              application_surface_motion_next_wake() == 382,
          "host motion: no-input frame advances choice visibility and consumes the old deadline");
    bool acknowledged = true;
    for (u32 query = 0; query < 4; ++query)
        acknowledged = acknowledged && !application_surface_motion_request_frame_if_due(366, true) &&
                       !g_app.frame_pending;
    check(acknowledged,
          "host motion: a later rejected Settings preparation cannot spin on the acknowledged opacity wake");
    check(application_surface_motion_request_frame_if_due(470, true),
          "host motion: no-input completion remains admitted through normal frame request");
    g_app.frame_pending = false;
    application_surface_motion_synchronize(470);
    check(g_app.renderer.layer_opacity.settings_choice == 1 && !application_surface_motion_next_wake() &&
              !application_surface_motion_request_frame_if_due(500, true) && !g_app.frame_pending,
          "host motion: completed choice entrance retains full visibility with no idle frames");
    g_app.renderer.settings_scene = nullptr;
    popup.active = false;
    UiMediaPickerProjection projection{};
    UiMediaPickerContent content{};
    content.account_scope = content.query_generation = 1;
    content.revision = 1;
    UiMediaPickerScene gallery{};
    gallery.projection = &projection;
    gallery.content = &content;
    gallery.account_scope = gallery.query_generation = 1;
    gallery.content_revision = 1;
    gallery.panel = {200, 200, 400, 400};
    gallery.active = true;
    g_app.renderer.media_picker_scene = &gallery;
    application_surface_motion_synchronize(500);
    check(g_app.renderer.layer_opacity.gallery == 0 && application_surface_motion_next_wake() == 516,
          "host motion: shared current gallery predicate admits native entrance");
    popup.kind = PopupKind::context_menu;
    popup.active = true;
    ++popup.opens;
    application_surface_motion_synchronize(510);
    check(g_app.renderer.layer_opacity.gallery == 1 && g_app.renderer.layer_opacity.popup == 0 &&
              application_surface_motion_next_wake() == 526,
          "host motion: covering popup settles the gallery while owning its own wake");
    popup.active = false;
    application_surface_motion_synchronize(511);
    check(!application_surface_motion_next_wake() && g_app.renderer.layer_opacity.gallery == 1,
          "host motion: uncovering has no residual deadline or replay");
    application_surface_motion_retire(RendererLayerSurface::gallery);
    application_surface_motion_synchronize(600);
    check(g_app.renderer.layer_opacity.gallery == 0,
          "host motion: explicit close/reopen binds fresh scope");
    show(window, SW_SHOWMINNOACTIVE);
    check(IsIconic(window) && !application_surface_motion_next_wake(),
          "host motion: actual minimized HWND has no visual deadline");
    application_surface_motion_synchronize(601);
    check(g_app.renderer.layer_opacity.gallery == 1, "host motion: minimized trajectory settles");
    retire_all();
    g_app.renderer.media_picker_scene = nullptr;
    g_app.renderer.popup_scene = nullptr;
    show(window, SW_SHOWNOACTIVATE);
    ProfileScene profile{};
    profile.active = true;
    profile.self_id = 42;
    profile.opens = 1;
    profile.rectangle = {40, 40, 300, 400};
    // Popouts slide in opaque; the full profile modal owns the fade.
    g_app.renderer.profile_scene = &profile;
    application_surface_motion_synchronize(690);
    check(g_app.renderer.layer_opacity.profile == 1, "host motion: profile popout enters without a fade");
    application_surface_motion_retire(RendererLayerSurface::profile);
    profile.presentation = ProfilePresentation::modal;
    const auto& profile_pose = g_app.renderer.layer_transform[static_cast<u32>(RendererLayerSurface::profile)];
    application_surface_motion_synchronize(700);
    check(g_app.renderer.layer_opacity.profile == 0 && application_surface_motion_next_wake() == 716 &&
              profile_pose.scale == kUiMotionModalScale && profile_pose.origin_x == 170 &&
              profile_pose.origin_y == 220,
          "host motion: the full profile modal enters faded and scaled about its centre with a frame wake");
    ++profile.revision;
    application_surface_motion_synchronize(760);
    const float entering = g_app.renderer.layer_opacity.profile;
    check(entering > 0 && entering < 1 &&
              profile_pose.scale == kUiMotionModalScale + (1 - kUiMotionModalScale) * entering,
          "host motion: async profile publication does not restart the modal spring; scale follows opacity");
    application_surface_motion_synchronize(1100);
    check(g_app.renderer.layer_opacity.profile == 1 && profile_pose.scale == 1 &&
              !application_surface_motion_next_wake(),
          "host motion: the modal settles at identity after its 400 ms spring and retires frame scheduling");
    // The exit: fade and shrink on the exit spring, then the owner's close runs once.
    closing_profile = &profile;
    profile_closes = 0;
    g_app.frame_pending = false;
    check(application_surface_motion_exit(RendererLayerSurface::profile, close_profile) && g_app.frame_pending &&
              application_surface_motion_exit(RendererLayerSurface::profile, close_profile) && !profile_closes,
          "host motion: a modal exit starts once, keeps the surface and requests a frame");
    application_surface_motion_synchronize(1110);
    check(g_app.renderer.layer_opacity.profile == 1 && application_surface_motion_next_wake() == 1126,
          "host motion: the exit starts from the shown modal at the next frame");
    application_surface_motion_synchronize(1200);
    const float leaving = g_app.renderer.layer_opacity.profile;
    check(leaving > 0 && leaving < 1 &&
              profile_pose.scale == kUiMotionModalScale + (1 - kUiMotionModalScale) * leaving && !profile_closes,
          "host motion: the exiting modal fades and shrinks together");
    application_surface_motion_synchronize(1350);
    check(profile_closes == 1 && !profile.active && g_app.renderer.layer_opacity.profile == 1 &&
              profile_pose.scale == 1 && !application_surface_motion_next_wake(),
          "host motion: the finished exit closes the owner once and leaves identity and no wake");
    profile.active = true;
    ++profile.opens;
    profile_closes = 0;
    application_surface_motion_synchronize(1400);
    application_surface_motion_synchronize(1800);
    check(application_surface_motion_exit(RendererLayerSurface::profile, close_profile) && !profile_closes,
          "host motion: a reopened modal can exit again");
    application_surface_motion_synchronize(1810);
    application_surface_motion_finish_exits();
    check(profile_closes == 1 && !profile.active && g_app.renderer.layer_opacity.profile == 1,
          "host motion: input finishes a pending exit at once");
    // A surface covered while it exits (a settings dialog opened over it) closes at once.
    profile.active = true;
    ++profile.opens;
    profile_closes = 0;
    application_surface_motion_synchronize(1820);
    application_surface_motion_synchronize(2220);
    UiSettingsScene covering{};
    covering.active = true;
    check(application_surface_motion_exit(RendererLayerSurface::profile, close_profile),
          "host motion: the reopened modal starts another exit");
    g_app.renderer.settings_scene = &covering;
    application_surface_motion_synchronize(2230);
    g_app.renderer.settings_scene = nullptr;
    check(profile_closes == 1 && !profile.active && !application_surface_motion_next_wake(),
          "host motion: a modal covered while it exits closes at once");
    profile.active = true;
    ++profile.opens;
    g_app.preferences.reduced_motion = true;
    application_surface_motion_synchronize(1900);
    check(g_app.renderer.layer_opacity.profile == 1 && !application_surface_motion_next_wake() &&
              !application_surface_motion_exit(RendererLayerSurface::profile, close_profile),
          "host motion: reduced motion shows the modal at once and closes it without an exit");
    g_app.preferences.reduced_motion = false;
    profile.active = false;
    application_surface_motion_synchronize(1902);
    g_app.renderer.profile_scene = nullptr;
    // The server header popout grows from .01 about its top centre while it
    // fades in (the official scale popout, 120 ms CSS ease-out).
    const auto& popup_pose = g_app.renderer.layer_transform[static_cast<u32>(RendererLayerSurface::popup)];
    g_app.renderer.popup_scene = &popup;
    popup.kind = PopupKind::context_menu;
    popup.active = true;
    g_app.shell_scene.guild_menu_expanded = true;
    ++popup.opens;
    application_surface_motion_synchronize(2000);
    check(popup_pose.scale == .01f && popup_pose.origin_x == 100 && popup_pose.origin_y == 20 &&
              g_app.renderer.layer_opacity.popup == 0,
          "host motion: the server header popout starts scaled down at its top centre");
    application_surface_motion_synchronize(2060);
    const float opacity = g_app.renderer.layer_opacity.popup;
    check(opacity > 0 && opacity < 1 && popup_pose.scale == .01f + .99f * opacity,
          "host motion: popout scale and opacity share one eased trajectory");
    application_surface_motion_synchronize(2120);
    check(popup_pose.scale == 1 && g_app.renderer.layer_opacity.popup == 1 &&
              !application_surface_motion_next_wake(),
          "host motion: the popout settles at identity scale after 120 ms");
    g_app.shell_scene.guild_menu_expanded = false;
    ++popup.opens;
    application_surface_motion_synchronize(2200);
    check(popup_pose.scale == 1 && popup_pose.shift_y == 0 && g_app.renderer.layer_opacity.popup == 0,
          "host motion: context menus fade without scaling or sliding");
    // A popout below its anchor slides up to it from 10 DIPs, opaque, over 200 ms of CSS ease-out.
    popup.kind = PopupKind::popover;
    popup.anchor = {20, 0, 60, 16};
    ++popup.opens;
    application_surface_motion_synchronize(2300);
    check(g_app.renderer.layer_opacity.popup == 1 && popup_pose.shift_y == kUiMotionPopoutDistance &&
              popup_pose.shift_x == 0 && popup_pose.scale == 1,
          "host motion: a popout starts 10 DIPs away from its anchor at full opacity");
    application_surface_motion_synchronize(2400);
    check(popup_pose.shift_y > 0 && popup_pose.shift_y < kUiMotionPopoutDistance * .5f,
          "host motion: the popout slide eases out toward its anchor");
    application_surface_motion_synchronize(2500);
    check(popup_pose.shift_y == 0 && !application_surface_motion_next_wake(),
          "host motion: the popout rests at its anchor after 200 ms with no wake");
    // A tooltip pops in on its stiff spring: opacity and scale .95 about its centre.
    popup.kind = PopupKind::tooltip;
    ++popup.opens;
    application_surface_motion_synchronize(2600);
    check(g_app.renderer.layer_opacity.popup == 0 && popup_pose.scale == kUiMotionTooltipScale &&
              popup_pose.origin_x == 100 && popup_pose.origin_y == 80,
          "host motion: a tooltip starts transparent at 95% about its centre");
    application_surface_motion_synchronize(2760);
    check(g_app.renderer.layer_opacity.popup == 1 && popup_pose.scale == 1 && !application_surface_motion_next_wake(),
          "host motion: the tooltip spring settles within 160 ms");
    popup.active = false;
    retire_all();
    g_app.renderer.popup_scene = nullptr;
    shared_scheduler();
    background_host(window);
    g_app.renderer_ready = false;
    g_app.window = nullptr;
    g_window_state = {};
    DestroyWindow(window);
    UnregisterClassW(name, type.hInstance);
}
} // namespace maicord::surface_motion_host_test

namespace maicord {
bool renderer_set_background_phase(D3D11Renderer* renderer, float phase) noexcept {
    if (!renderer)
        return false;
    renderer->background_phase_seconds = phase;
    return true;
}
namespace app_detail {
bool application_settings_active() noexcept {
    return surface_motion_host_test::settings_active;
}
bool application_theme_editor_active() noexcept {
    return false;
}
u64 application_unread_banner_next_wake() noexcept {
    return 0;
}
u64 application_guild_folders_next_wake() noexcept {
    return 0;
}
bool application_effects_selected() noexcept {
    return false;
}
bool application_effects_motion_allowed() noexcept {
    return surface_motion_host_test::effect_motion_allowed;
}
u64 application_effects_transition_wake() noexcept {
    return surface_motion_host_test::effect_transition_wake;
}
bool application_effects_preview_running() noexcept {
    return false;
}
u32 application_effects_interval_ms() noexcept {
    return 42;
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_surface_motion_host_entry() noexcept {
    maicord::surface_motion_host_test::run();
    maicord::test_exit(maicord::surface_motion_host_test::failures, "surface motion host");
}
