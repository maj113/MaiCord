#include "application_downloads_inactive.hpp"
#include "application_inbox_inactive.hpp"
#include "application_state.hpp"
#include "application_surface_motion.hpp"
#include "application_background.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/profile_scene.hpp"

namespace maicord::surface_motion_host_test {
using namespace app_detail;
u32 failures{};
bool settings_active{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
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
    ShowWindow(window, SW_HIDE);
    check(!application_background_next_wake(10000) && !r.background_motion_running &&
              !r.background_next_frame_ms && r.background_phase_seconds == 3.f,
          "background host: hidden native window retires overdue shader deadline without presentation");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    check(
        IsWindowVisible(window) && !IsIconic(window) && application_background_next_wake(10000) == 10000 &&
            application_background_synchronize(10000) && application_background_next_wake(10001) == 10042 &&
            application_background_next_wake(10041) == 10042 && r.background_phase_seconds == 3.f,
        "background host: visible native window arms a stable future deadline without warm phase drift");
    check(application_background_synchronize(10042) && r.background_next_frame_ms == 10084 &&
              r.background_phase_seconds > 3.041f && r.background_phase_seconds < 3.043f,
          "background host: due production sample advances one bounded shader phase");
    const float paused = r.background_phase_seconds;
    ShowWindow(window, SW_HIDE);
    check(!application_background_next_wake(10043) && !r.background_motion_running &&
              !r.background_next_frame_ms && r.background_phase_seconds == paused,
          "background host: hide without synchronize clears running clock and preserves shader phase");
    ShowWindow(window, SW_SHOWNOACTIVATE);
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
    g_app.window = window;
    g_app.renderer_ready = true;
    g_app.renderer.width = g_app.renderer.height = 640;
    g_app.renderer.dpi = 96;
    g_app.renderer.layer_opacity = {1, 1, 1, 1, 1, 1};
    g_app.surface_motion.system_preference_cached = true;
    g_app.surface_motion.client_area_animation = true;
    auto& popup = g_app.popup_scene;
    popup.kind = PopupKind::context_menu;
    popup.rectangle = {20, 20, 180, 140};
    popup.owner_id = 7;
    popup.opens = 1;
    popup.active = true;
    g_app.renderer.popup_scene = &popup;
    application_surface_motion_synchronize(10);
    check(g_app.renderer.layer_opacity.popup == 1 && !application_surface_motion_next_wake(10),
          "host motion: actual hidden HWND suppresses entrance and wake");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    ++popup.opens;
    application_surface_motion_synchronize(100);
    check(g_app.renderer.layer_opacity.popup == 0 && application_surface_motion_next_wake(100) == 116 &&
              application_surface_motion_next_wake(130) == 116,
          "host motion: actual visible HWND binds an absolute entrance deadline");
    application_surface_motion_synchronize(160);
    check(g_app.renderer.layer_opacity.popup == .875f && g_app.renderer.layer_opacity.gallery == 1 &&
              g_app.renderer.layer_opacity.settings_choice == 1,
          "host motion: production adapter samples shared easing in the exact draw factor");
    g_app.preferences.reduced_motion = true;
    check(!application_surface_motion_next_wake(161), "host motion: reduced preference suppresses wake");
    application_surface_motion_synchronize(161);
    check(g_app.renderer.layer_opacity.popup == 1, "host motion: reduced preference settles immediately");
    g_app.preferences.reduced_motion = false;
    ++popup.opens;
    application_surface_motion_synchronize(200);
    g_app.renderer.live_resize = true;
    check(!application_surface_motion_next_wake(201), "host motion: live sizing suppresses wake");
    application_surface_motion_synchronize(201);
    check(g_app.renderer.layer_opacity.popup == 1, "host motion: live sizing settles draw state");
    g_app.renderer.live_resize = false;
    g_app.surface_motion.client_area_animation = false;
    ++popup.opens;
    application_surface_motion_synchronize(300);
    check(g_app.renderer.layer_opacity.popup == 1 && !application_surface_motion_next_wake(300),
          "host motion: cached Windows motion preference prevents a trajectory");
    g_app.surface_motion.client_area_animation = true;
    UiSettingsScene settings{};
    settings.active = true;
    settings.page_generation = 1;
    g_app.renderer.settings_scene = &settings;
    popup.active = false;
    application_surface_motion_synchronize(350);
    g_app.frame_pending = false;
    check(g_app.renderer.layer_opacity.settings_page == 0 &&
              application_surface_motion_next_wake(350) == 366,
          "host motion: initial Settings page binds zero-opacity entrance and independent cadence");
    bool before_due = true;
    for (u32 query = 0; query < 4; ++query)
        before_due = before_due && !application_surface_motion_request_frame_if_due(365, true) &&
                     !g_app.frame_pending;
    check(before_due && !application_surface_motion_request_frame_if_due(366, false) &&
              !g_app.frame_pending && application_surface_motion_next_wake(366) == 366,
          "host motion: early or unavailable display slots cannot enqueue Settings opacity ticks");
    check(application_surface_motion_request_frame_if_due(366, true) && g_app.frame_pending,
          "host motion: due Settings entrance requests a pending frame without hover or account input");
    // Production samples at frame entry, before later projection can fail.
    g_app.frame_pending = false;
    application_surface_motion_synchronize(366);
    check(g_app.renderer.layer_opacity.settings_page > 0 &&
              g_app.renderer.layer_opacity.settings_page < 1 &&
              application_surface_motion_next_wake(366) == 382,
          "host motion: no-input frame advances Settings visibility and consumes the old deadline");
    bool acknowledged = true;
    for (u32 query = 0; query < 4; ++query)
        acknowledged = acknowledged && !application_surface_motion_request_frame_if_due(366, true) &&
                       !g_app.frame_pending;
    check(
        acknowledged,
        "host motion: a later rejected Settings preparation cannot spin on the acknowledged opacity wake");
    check(application_surface_motion_request_frame_if_due(470, true),
          "host motion: no-input completion remains admitted through normal frame request");
    g_app.frame_pending = false;
    application_surface_motion_synchronize(470);
    check(g_app.renderer.layer_opacity.settings_page == 1 && !application_surface_motion_next_wake(470) &&
              !application_surface_motion_request_frame_if_due(500, true) && !g_app.frame_pending,
          "host motion: completed Settings entrance retains full visibility with no idle frames");
    popup.active = true;
    popup.kind = PopupKind::settings_choice;
    ++popup.opens;
    application_surface_motion_synchronize(400);
    check(g_app.renderer.layer_opacity.settings_choice == 0 && g_app.renderer.layer_opacity.popup == 1 &&
              application_surface_motion_next_wake(430) == 416,
          "host motion: settings choice owns its factor and exact deadline");
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
    check(g_app.renderer.layer_opacity.gallery == 0 && application_surface_motion_next_wake(550) == 516,
          "host motion: shared current gallery predicate admits native entrance");
    popup.kind = PopupKind::context_menu;
    popup.active = true;
    ++popup.opens;
    application_surface_motion_synchronize(510);
    check(g_app.renderer.layer_opacity.gallery == 1 && g_app.renderer.layer_opacity.popup == 0 &&
              application_surface_motion_next_wake(530) == 526,
          "host motion: covering popup settles the gallery while owning its own wake");
    popup.active = false;
    application_surface_motion_synchronize(511);
    check(!application_surface_motion_next_wake(511) && g_app.renderer.layer_opacity.gallery == 1,
          "host motion: uncovering has no residual deadline or replay");
    application_surface_motion_retire(RendererLayerSurface::gallery);
    application_surface_motion_synchronize(600);
    check(g_app.renderer.layer_opacity.gallery == 0,
          "host motion: explicit close/reopen binds fresh scope");
    ShowWindow(window, SW_SHOWMINNOACTIVE);
    check(IsIconic(window) && !application_surface_motion_next_wake(601),
          "host motion: actual minimized HWND has no visual deadline");
    application_surface_motion_synchronize(601);
    check(g_app.renderer.layer_opacity.gallery == 1, "host motion: minimized trajectory settles");
    application_surface_motion_retire_all();
    g_app.renderer.media_picker_scene = nullptr;
    g_app.renderer.popup_scene = nullptr;
    ShowWindow(window, SW_SHOWNOACTIVATE);
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
    application_surface_motion_synchronize(700);
    check(g_app.renderer.layer_opacity.profile == 0 && application_surface_motion_next_wake(700) == 716,
          "host motion: user profile popout owns a fresh fade and native frame wake");
    ++profile.revision;
    application_surface_motion_synchronize(760);
    check(g_app.renderer.layer_opacity.profile == .875f,
          "host motion: async profile publication does not restart its entrance");
    application_surface_motion_synchronize(820);
    check(g_app.renderer.layer_opacity.profile == 1 && !application_surface_motion_next_wake(820),
          "host motion: profile fade settles and retires frame scheduling");
    application_surface_motion_retire(RendererLayerSurface::profile);
    ++profile.opens;
    application_surface_motion_synchronize(900);
    check(g_app.renderer.layer_opacity.profile == 0,
          "host motion: profile close/reopen replays even with the same cached user");
    g_app.preferences.reduced_motion = true;
    application_surface_motion_synchronize(901);
    check(g_app.renderer.layer_opacity.profile == 1 && !application_surface_motion_next_wake(901),
          "host motion: reduced-motion profile settles without a pending timer");
    g_app.preferences.reduced_motion = false;
    profile.active = false;
    application_surface_motion_synchronize(902);
    g_app.renderer.profile_scene = nullptr;
    // The server header popout grows from .01 about its top centre while it
    // fades in (the official scale popout, 120 ms CSS ease-out).
    g_app.renderer.popup_scene = &popup;
    popup.kind = PopupKind::context_menu;
    popup.active = true;
    g_app.shell_scene.guild_menu_expanded = true;
    ++popup.opens;
    application_surface_motion_synchronize(1000);
    check(g_app.renderer.popup_layer_scale == .01f && g_app.renderer.popup_layer_origin_x == 100 &&
              g_app.renderer.popup_layer_origin_y == 20 && g_app.renderer.layer_opacity.popup == 0,
          "host motion: the server header popout starts scaled down at its top centre");
    application_surface_motion_synchronize(1060);
    const float opacity = g_app.renderer.layer_opacity.popup;
    check(opacity > 0 && opacity < 1 && g_app.renderer.popup_layer_scale == .01f + .99f * opacity,
          "host motion: popout scale and opacity share one eased trajectory");
    application_surface_motion_synchronize(1120);
    check(g_app.renderer.popup_layer_scale == 1 && g_app.renderer.layer_opacity.popup == 1 &&
              !application_surface_motion_next_wake(1120),
          "host motion: the popout settles at identity scale after 120 ms");
    g_app.shell_scene.guild_menu_expanded = false;
    ++popup.opens;
    application_surface_motion_synchronize(1200);
    check(g_app.renderer.popup_layer_scale == 1 && g_app.renderer.layer_opacity.popup == 0,
          "host motion: ordinary popups fade without scaling");
    popup.active = false;
    application_surface_motion_retire_all();
    g_app.renderer.popup_scene = nullptr;
    background_host(window);
    g_app.renderer_ready = false;
    g_app.window = nullptr;
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
u64 application_unread_banner_next_wake(u64) noexcept {
    return 0;
}
u64 application_guild_folders_next_wake(u64) noexcept {
    return 0;
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_surface_motion_host_entry() noexcept {
    maicord::surface_motion_host_test::run();
    const bool failed = maicord::surface_motion_host_test::failures != 0;
    constexpr char pass[] = "PASS surface motion host\n", fail[] = "FAIL surface motion host\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
