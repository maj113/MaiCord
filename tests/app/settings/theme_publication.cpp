#include "shell/state.hpp"
#include "shell/window_state.hpp"
#include "settings/custom_theme.hpp"
#include "effects/effects.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "../../support/test_output.hpp"

// The shipping theme publication (src/app/settings/theme_publication.cpp) under a fixture clock: its glides step once
// per display frame, settle exactly on the new look and then ask for nothing; reduced motion and a hidden window
// show a change at once. The renderer's fonts and the theme consumers are receipts; nothing is drawn.
namespace maicord::app_detail {
u64 theme_fixture_now() noexcept;
u64 theme_fixture_now_us() noexcept;
bool theme_fixture_refresh(bool typography_changed, bool geometry_changed) noexcept;
bool theme_fixture_prepare_typography(D3D11Renderer*, const UiThemeTypography&, RendererTypographyCandidate*) noexcept;
bool theme_fixture_commit_typography(D3D11Renderer*, RendererTypographyCandidate*) noexcept;
void theme_fixture_discard_typography(RendererTypographyCandidate*) noexcept;
u32 theme_fixture_typography_revision(const D3D11Renderer&) noexcept;
} // namespace maicord::app_detail
#define clock_monotonic_milliseconds theme_fixture_now
#define application_now_us theme_fixture_now_us
#define application_refresh_theme_consumers theme_fixture_refresh
#define renderer_prepare_typography theme_fixture_prepare_typography
#define renderer_commit_typography theme_fixture_commit_typography
#define renderer_discard_typography theme_fixture_discard_typography
#define renderer_typography_revision theme_fixture_typography_revision
#include "../../../src/app/settings/theme_publication.cpp"

namespace maicord {
namespace theme_publication_test {
u32 failures{};
u64 now = 100'000;
u32 refreshes{};
// The effects' clock and the theme's effect's knobs (month, sky), as application_effects_* answers them.
EffectLocalTime local{2026, 3, 1, 3, 0};
float month_knob = -1, sky_knob = -1;
const ClientThemeDocument* document{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
} // namespace theme_publication_test

namespace app_detail {
u64 theme_fixture_now() noexcept {
    return theme_publication_test::now;
}
// The glides' clock (the performance counter, application_now_us) runs with the loop's here.
u64 theme_fixture_now_us() noexcept {
    return theme_publication_test::now * 1000;
}
bool theme_fixture_refresh(bool, bool) noexcept {
    ++theme_publication_test::refreshes;
    return application_request_frame();
}
bool theme_fixture_prepare_typography(D3D11Renderer*, const UiThemeTypography&, RendererTypographyCandidate*) noexcept {
    return true;
}
bool theme_fixture_commit_typography(D3D11Renderer*, RendererTypographyCandidate*) noexcept {
    return true;
}
void theme_fixture_discard_typography(RendererTypographyCandidate*) noexcept {
}
u32 theme_fixture_typography_revision(const D3D11Renderer&) noexcept {
    return 1;
}
float application_effects_drawn_knob(const char*, const char* id) noexcept {
    return id[0] == 'm' ? theme_publication_test::month_knob : theme_publication_test::sky_knob;
}
EffectLocalTime application_effects_local_time(u64 now_ms, u64* next_ms) noexcept {
    *next_ms = now_ms + 60'000;
    return theme_publication_test::local;
}
const ClientThemeDocument* application_custom_theme_document() noexcept {
    return theme_publication_test::document;
}
} // namespace app_detail

namespace theme_publication_test {
using namespace app_detail;
u32 scratch[4096];
ClientThemeDocument linked{};

// The look a publication of the current source settles on.
Theme settled_look() noexcept {
    Theme look = document ? client_theme_document_at(*document, local.hour * 60u + local.minute, local.month, local.day)
                          : client_theme_projection(g_app.preferences);
    ui_theme_resolve_emphasis(&look);
    return look;
}
bool shows(const Theme& look) noexcept {
    const Theme& shown = application_theme();
    return ui_theme_palette_equal(shown, look) && shown.appearance == look.appearance &&
           background_theme_equal(shown.background, look.background);
}
// Display frames every `frame_ms` until the glide settles: whether each frame published exactly once and moved
// base_low.red one way, and how many frames it took.
struct Run {
    u32 frames;
    bool every_frame, monotonic;
};
Run run_frames(u32 frame_ms) noexcept {
    Run run{0, true, true};
    float before = application_theme().base_low.red;
    const bool rising = settled_look().base_low.red > before;
    while (application_theme_transition_active() && run.frames < 1000) {
        now += frame_ms;
        const u32 published = refreshes;
        application_theme_schedule_tick(now, true);
        const float red = application_theme().base_low.red;
        run.every_frame = run.every_frame && refreshes == published + 1;
        run.monotonic = run.monotonic && (rising ? red > before : red < before);
        before = red;
        ++run.frames;
    }
    return run;
}
// Ticks that ask for no more frames once nothing glides.
bool quiet(u32 ticks) noexcept {
    g_app.frame_pending = false;
    const u32 published = refreshes;
    for (u32 i = 0; i < ticks; ++i) {
        now += 16;
        application_theme_schedule_tick(now, true);
    }
    return refreshes == published && !g_app.frame_pending && !application_theme_transition_active();
}
bool switch_builtin(ClientTheme theme) noexcept {
    g_app.preferences.theme = theme;
    return application_custom_theme_publish(nullptr, true);
}
// The glide itself (client_theme_glide): the colours on the ease-in-out curve, a gradient stop by stop, fading against
// a flat look or through flat between different gradients; a change of effect shows at once.
void glide_looks() noexcept {
    Theme from = kDefaultTheme, to = kLightTheme;
    from.background = {{0x000000, 0x0000ff, 0, 0, 0}, 90, 2, 40, BackgroundEffect::none, 50, 25, 35};
    to.background = {{0xffffff, 0xff0000, 0, 0, 0}, 270, 2, 80, BackgroundEffect::none, 50, 25, 35};
    Theme mid = to, end = to;
    const bool running = client_theme_glide(&mid, from, 300, 600);
    const auto& b = mid.background;
    check(running && !client_theme_glide(&end, from, 600, 600) && bytes_equal(&end, &to, sizeof(Theme)) &&
              mid.base_low.red - (from.base_low.red + to.base_low.red) * .5f < 1e-5f &&
              (from.base_low.red + to.base_low.red) * .5f - mid.base_low.red < 1e-5f && b.colors[0] == 0x808080 &&
              b.colors[1] == 0x800080 && b.angle_degrees == 180 && b.intensity == 60 && b.color_count == 2,
          "theme glide: halfway the colours and a gradient's stops, angle and intensity are halfway; at the end, whole");
    Theme flat = kDefaultTheme, fading = to, three_stops = to;
    flat.background = {{}, 0, 0, 0, BackgroundEffect::none, 50, 25, 35};
    const bool fade_in = client_theme_glide(&fading, flat, 300, 600);
    three_stops.background.colors[2] = 0x00ff00;
    three_stops.background.color_count = 3;
    Theme crossing = three_stops;
    client_theme_glide(&crossing, from, 150, 600);
    const bool outgoing =
        bytes_equal(crossing.background.colors, from.background.colors, sizeof(from.background.colors)) &&
        crossing.background.intensity > 0 && crossing.background.intensity < 40;
    crossing = three_stops;
    client_theme_glide(&crossing, from, 450, 600);
    const bool incoming = crossing.background.color_count == 3 && crossing.background.intensity > 0 &&
                          crossing.background.intensity < 80;
    Theme starry = to;
    starry.background = flat.background;
    starry.background.effect = BackgroundEffect::stars;
    client_theme_glide(&starry, from, 300, 600);
    check(fade_in && fading.background.color_count == 2 && fading.background.intensity == 40 && outgoing && incoming &&
              starry.background.effect == BackgroundEffect::stars && !starry.background.color_count,
          "theme glide: a gradient fades in over a flat look, passes through flat to another gradient, and a change of "
          "effect shows at once");
}

void run() noexcept {
    glide_looks();
    g_window_state.visible = true;
    g_window_state.minimized = false;
    g_app.renderer.has_presented = true;
    g_app.surface_motion.system_preference_cached = g_app.surface_motion.client_area_animation = true;
    g_app.preferences = client_preferences_defaults();
    g_app.preferences.reduced_motion = false;
    check(switch_builtin(ClientTheme::dark) && !application_theme_transition_active() && shows(settled_look()),
          "theme glide: the first publication shows at once");

    // Dark to Light at 60 Hz: one publication a frame, every frame a step on, exactly Light at the end.
    const Theme dark = application_theme();
    check(switch_builtin(ClientTheme::light) && application_theme_transition_active() &&
              ui_theme_palette_equal(application_theme(), dark),
          "theme glide: a built-in theme change starts a glide from the look that showed");
    const u32 published = refreshes;
    application_theme_schedule_tick(now, false);
    check(refreshes == published && application_theme_transition_active(),
          "theme glide: a loop turn without a display frame publishes nothing");
    const Run sixty = run_frames(16);
    test_write_metric("theme glide frames at 60 Hz: ", sixty.frames);
    check(sixty.every_frame && sixty.monotonic && sixty.frames == (kClientThemeGlideMs + 15) / 16 &&
              shows(settled_look()),
          "theme glide: a change the user makes publishes on every display frame for 0.6 s and settles exactly");
    check(quiet(30), "theme glide: a settled glide publishes nothing and asks for no frame");

    // Back to Dark at 144 Hz: the steps follow the display, not a timer.
    check(switch_builtin(ClientTheme::dark) && application_theme_transition_active(), "theme glide: back to Dark");
    const Run fast = run_frames(7);
    test_write_metric("theme glide frames at 144 Hz: ", fast.frames);
    check(fast.every_frame && fast.monotonic && fast.frames == (kClientThemeGlideMs + 6) / 7 && shows(settled_look()),
          "theme glide: a faster display gets a publication on each of its frames");

    // Retargeted halfway: the glide starts again from what shows, without a jump.
    check(switch_builtin(ClientTheme::light), "theme glide: to Light again");
    for (u32 i = 0; i < 18; ++i) {
        now += 16;
        application_theme_schedule_tick(now, true);
    }
    const float midway = application_theme().base_low.red;
    check(switch_builtin(ClientTheme::dark) && application_theme().base_low.red == midway &&
              run_frames(16).frames == (kClientThemeGlideMs + 15) / 16 && shows(settled_look()),
          "theme glide: a change during a glide glides on from the look that shows");

    // Reduced motion, Windows' animations off, a hidden window: at once.
    g_app.preferences.reduced_motion = true;
    check(switch_builtin(ClientTheme::light) && !application_theme_transition_active() && shows(settled_look()) &&
              quiet(5),
          "theme glide: reduced motion shows a change at once");
    g_app.preferences.reduced_motion = false;
    g_app.surface_motion.client_area_animation = false;
    check(switch_builtin(ClientTheme::dark) && !application_theme_transition_active() && shows(settled_look()),
          "theme glide: Windows' animations turned off show a change at once");
    g_app.surface_motion.client_area_animation = true;
    check(switch_builtin(ClientTheme::light) && application_theme_transition_active(), "theme glide: to Light");
    now += 100;
    application_theme_schedule_tick(now, true);
    g_window_state.minimized = true;
    now += 16;
    application_theme_schedule_tick(now, false);
    check(!application_theme_transition_active() && shows(settled_look()) && quiet(5),
          "theme glide: a window that can't show it settles at once");
    g_window_state.minimized = false;
    // A saved theme loaded at startup shows at once.
    g_app.preferences.theme = ClientTheme::dark;
    check(application_custom_theme_publish(nullptr, false) && !application_theme_transition_active() &&
              shows(settled_look()),
          "theme glide: a publication that doesn't animate shows at once");

    // A theme with variants that follows its effect's knobs (03:00 on 1 March: its night look).
    const auto source = utf8_literal(
        "{\"version\":\"2.0\",\"name\":\"Linked\",\"background\":{\"effect\":\"silk-waves\",\"follow\":{\"month\":"
        "\"month\",\"phase\":\"sky\",\"phases\":[\"auto\",\"auto\",\"day\",\"night\"]}},\"variants\":{"
        "\"night\":{\"colors\":{\"base_low\":\"#050608\"},\"months\":[{},{},{\"accent\":\"#ff8040\"},{},{},{},{},{},{},{},"
        "{},{}]},\"day\":{\"base\":\"light\",\"colors\":{\"base_low\":\"#f4f1ea\"}}}}");
    check(client_theme_document_parse(source, client_theme_document_defaults(), {scratch, 4096}, &linked).ok(),
          "theme glide: the linked theme reads");
    document = &linked;
    check(application_custom_theme_publish(document, true) && application_theme_transition_active() &&
              run_frames(16).frames == (kClientThemeGlideMs + 15) / 16 && shows(settled_look()),
          "theme glide: applying a theme with variants glides to it like any other");
    // Day and night held on Day: the schedule is due at once and the theme glides to its day look as the user's change.
    sky_knob = 2;
    application_theme_schedule_tick(now, false);
    local.hour = 12; // the held phase: client_theme_follow puts the clock at noon
    const Run held = run_frames(16);
    check(held.every_frame && held.monotonic && held.frames == (kClientThemeGlideMs + 15) / 16 && shows(settled_look()) &&
              application_theme().appearance == ThemeAppearance::light,
          "theme glide: a knob the theme follows glides it to the held phase in 0.6 s");
    // Back to the clock, which jumps to 03:00: a light/dark flip the clock makes takes the effects' appearance glide.
    sky_knob = 0;
    local.hour = 3;
    now += 60'000;
    application_theme_schedule_tick(now, false);
    const Run flip = run_frames(16);
    check(flip.every_frame && flip.frames == (kClientThemeGlideMs + 15) / 16 && shows(settled_look()),
          "theme glide: leaving a held phase is the user's change too");
    // The clock alone: a jump from night to noon glides over kEffectAmbientEaseMs.
    local.hour = 12;
    now += 60'000;
    application_theme_schedule_tick(now, false);
    const Run jump = run_frames(16);
    test_write_metric("theme glide frames for a jump of the clock: ", jump.frames);
    check(jump.every_frame && jump.monotonic && jump.frames == (kEffectAmbientEaseMs + 15) / 16 &&
              shows(settled_look()) && quiet(10),
          "theme glide: a jump of the clock glides over the effects' appearance glide, then asks for nothing");
    document = nullptr;
    application_theme_release();
}
} // namespace theme_publication_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_theme_publication_entry() noexcept {
    maicord::theme_publication_test::run();
    maicord::test_exit(maicord::theme_publication_test::failures, "theme publication");
}
