#include "maicord/app/settings/client_theme.hpp"
#include "maicord/app/shell/background_frame_policy.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/text/content_text_retention.hpp"
#include "maicord/render/effects/background_effect.hpp"
#include "maicord/render/effects/background_fx.hpp"
#include "maicord/ui/theme/background_animation.hpp"
#include "maicord/ui/theme/background_surface.hpp"

namespace maicord {
void test_background_effect_policy(void (*check)(bool, const char*) noexcept) noexcept {
    // Room for the ANSI code block palette, the slash command option chip
    // tokens and the list row highlight tokens.
    static_assert(sizeof(Theme) <= 2120);
    auto authored_dark = kDefaultTheme;
    authored_dark.text_primary = {0, 1, 0, 1};
    authored_dark.background = background_theme_defaults();
    auto authored_light = authored_dark;
    authored_light.appearance = ThemeAppearance::light;
    authored_light.text_primary = {1, 0, 0, 1};
    auto appearance_only = authored_dark;
    appearance_only.appearance = ThemeAppearance::light;
    const auto dark_canvas = ui_background_canvas_color(authored_dark, authored_dark.surface);
    const auto light_canvas = ui_background_canvas_color(authored_light, authored_light.surface);
    check(dark_canvas.red == 0 && light_canvas.red == 1 &&
              ui_background_tier_opacity(authored_dark, UiBackgroundTier::chat) !=
                  ui_background_tier_opacity(authored_light, UiBackgroundTier::chat) &&
              content_text_theme_signature(authored_dark) != content_text_theme_signature(appearance_only),
          "theme appearance: authored foreground hues never choose canvas or text policy");
    BackgroundFrameWork phase{};
    phase.background_requested = true;
    u32 phase_reuses = 0;
    for (u32 frame = 0; frame < 64; ++frame)
        phase_reuses += background_frame_reuses_static_media(phase) ? 1u : 0u;
    bool BackgroundFrameWork::* changes[]{
        &BackgroundFrameWork::previous_damage,     &BackgroundFrameWork::account_work,
        &BackgroundFrameWork::dispatched_messages, &BackgroundFrameWork::media_animation_due,
        &BackgroundFrameWork::video_due,           &BackgroundFrameWork::smooth_scrolling,
        &BackgroundFrameWork::other_visual_due,    &BackgroundFrameWork::media_scan_pending};
    bool all_changes_pump = !background_frame_reuses_static_media({});
    for (auto change : changes) {
        auto changed = phase;
        changed.*change = true;
        all_changes_pump = all_changes_pump && !background_frame_reuses_static_media(changed);
    }
    auto completion = phase;
    completion.account_work = true;
    check(phase_reuses == 64 && all_changes_pump && !background_frame_reuses_static_media(completion) &&
              background_frame_reuses_static_media(phase),
          "background media policy: 64 shader phases reuse; completion, input, damage, scroll and explicit "
          "deadlines retain pumping");
    constexpr ClientTheme kinds[]{ClientTheme::dark, ClientTheme::amoled, ClientTheme::light};
    const Theme amoled_theme = ui_theme_amoled();
    const Theme* bases[]{&kDefaultTheme, &amoled_theme, &kLightTheme};
    constexpr BackgroundEffect effects[]{BackgroundEffect::stars, BackgroundEffect::comets,
                                         BackgroundEffect::stars_and_comets};
    static_assert(sizeof(effects) / sizeof(effects[0]) == kBackgroundEffectProfileCount);
    // The catalog is exactly None plus these three; nothing else is admitted.
    bool catalog = kBackgroundEffectCount == 4 && background_effect_known(0) && !background_effect_known(4);
    for (u32 i = 0; i < 3; ++i) {
        auto catalog_entry = background_theme_defaults();
        catalog_entry.effect = effects[i];
        catalog = catalog && static_cast<u32>(effects[i]) == i + 1 && background_effect_known(i + 1) &&
                  background_effect_profile_index(effects[i]) == i && background_theme_valid(catalog_entry);
    }
    auto removed_entry = background_theme_defaults();
    removed_entry.effect = static_cast<BackgroundEffect>(kBackgroundEffectCount);
    check(catalog && !background_theme_valid(removed_entry) &&
              background_effect_profile_index(BackgroundEffect::none) == kBackgroundEffectProfileCount,
          "background effect catalog: exactly Stars, Comets and Stars and comets plus None; any other id is "
          "rejected");
    PaintStream stream{};
    const bool stream_ready = paint_stream_initialize(&stream, 8);
    check(stream_ready, "flat effects: bounded native material stream initializes");
    if (!stream_ready)
        return;
    for (u32 mode = 0; mode < 3; ++mode) {
        ClientPreferences preferences = client_preferences_defaults();
        preferences.theme = kinds[mode];
        preferences.gradient.colors[0] = 0xff0099;
        preferences.gradient.colors[1] = 0x00ccff;
        preferences.gradient.colors[2] = 0x22ee55;
        preferences.gradient.angle_degrees = 270;
        preferences.gradient.intensity = 80;
        const auto palette = preferences.gradient;
        const u64 original_foreground = content_text_theme_signature(*bases[mode]);
        bool projection_identity = true, no_gradient_material = true, independent_effects = true;
        for (const auto effect : effects) {
            preferences.gradient.effect = effect;
            const auto settings = background_effect_settings_defaults(effect);
            preferences.gradient.speed = settings.speed;
            preferences.gradient.strength = settings.strength;
            preferences.gradient.density = settings.density;
            const ClientPreferences untouched = preferences;
            const Theme projected = client_theme_projection(preferences);
            projection_identity =
                projection_identity && bytes_equal(&projected, bases[mode], 20 * sizeof(Color)) &&
                content_text_theme_signature(projected) == original_foreground &&
                client_preferences_equal(preferences, untouched) && preferences.theme == kinds[mode];
            const auto& bg = projected.background;
            independent_effects = independent_effects && background_effects_valid(bg) &&
                                  background_render_valid(bg) && background_effects_active(bg) &&
                                  bg.effect == effect && bg.speed == settings.speed &&
                                  bg.strength == settings.strength && bg.density == settings.density &&
                                  !bg.color_count && !bg.intensity && !bg.angle_degrees;
            for (u32 i = 0; i < 5; ++i)
                independent_effects = independent_effects && !bg.colors[i] &&
                                      preferences.gradient.colors[i] == palette.colors[i];
            const bool reset = paint_stream_reset(&stream, 120, 60);
            const bool painted =
                reset && ui_background_paint_surface(&stream, {1, 2, 101, 42}, projected, projected.surface,
                                                     projected.interactive, 8, 1, .9f,
                                                     UiBackgroundSurfaceMode::gradient);
            const Color canvas = ui_background_canvas_color(projected, projected.base_low);
            no_gradient_material = no_gradient_material && painted && stream.count == 1 &&
                                   !ui_background_active(projected) && !background_theme_valid(bg) &&
                                   stream.quads[0].border_width == 1 &&
                                   bytes_equal(&canvas, &projected.base_low, sizeof(Color));
            const u32 canvas_first = stream.count;
            const bool canvas_painted = ui_background_paint_canvas(
                &stream, {1, 2, 101, 42}, projected, projected.base_low, UiBackgroundTier::chat);
            independent_effects =
                independent_effects && canvas_painted && stream.count == canvas_first + 1 &&
                stream.quads[canvas_first].border_width == -8 && !stream.quads[canvas_first].reserved_y &&
                bytes_equal(&stream.quads[canvas_first].fill, &projected.base_low, sizeof(Color));
        }
        check(projection_identity, "flat effects: every effect preserves all twenty Dark/AMOLED/Light "
                                   "tokens and foreground cache signature");
        check(independent_effects, "flat effects: projected effect-only descriptor strips palette while "
                                   "preserving exact saved palette and settings");
        check(no_gradient_material,
              "flat effects: explicit local gradient request remains ordinary opaque flat material");
        preferences.gradient.effect = BackgroundEffect::none;
        const auto disabled = client_theme_projection(preferences);
        check(bytes_equal(&disabled, bases[mode], 20 * sizeof(Color)) &&
                  content_text_theme_signature(disabled) == original_foreground &&
                  !ui_background_active(disabled) && !background_effects_active(disabled.background),
              "flat effects: disabling motion retains the original flat tokens without foreground "
              "invalidation");
    }
    paint_stream_release(&stream);

    constexpr ClientTheme gradient_kinds[]{ClientTheme::custom_gradient,
                                           ClientTheme::custom_light_gradient};
    constexpr u8 intensities[]{0, 20, 100};
    bool gradient_foreground_retained = true;
    for (const auto kind : gradient_kinds)
        for (const auto intensity : intensities) {
            auto preferences = client_preferences_defaults();
            preferences.theme = kind;
            preferences.gradient.intensity = intensity;
            preferences.gradient.effect = BackgroundEffect::none;
            const auto original = client_theme_projection(preferences);
            for (const auto selected_effect : effects) {
                preferences.gradient.effect = selected_effect;
                const auto projected = client_theme_projection(preferences);
                gradient_foreground_retained =
                    gradient_foreground_retained && ui_background_active(projected) &&
                    bytes_equal(&original, &projected, 20 * sizeof(Color)) &&
                    content_text_theme_signature(original) == content_text_theme_signature(projected) &&
                    projected.background.color_count == original.background.color_count &&
                    projected.background.angle_degrees == original.background.angle_degrees &&
                    projected.background.intensity == original.background.intensity;
                for (u32 stop = 0; stop < 5; ++stop)
                    gradient_foreground_retained =
                        gradient_foreground_retained &&
                        projected.background.colors[stop] == original.background.colors[stop];
            }
        }
    check(gradient_foreground_retained, "gradient effects: effect choice remains independent of actual "
                                        "color tokens, text cache and raw palette at0/20/100");

    BackgroundTheme effect{};
    effect.effect = BackgroundEffect::stars_and_comets;
    effect.speed = 28;
    effect.strength = 18;
    effect.density = 22;
    check(background_effects_valid(effect) && background_render_valid(effect) &&
              !background_theme_valid(effect),
          "flat effects: effect-only rendering admission never admits a zero-stop gradient");
    BackgroundTheme invalid = effect;
    invalid.colors[4] = 1;
    bool invalid_rejected = !background_effects_valid(invalid) && !background_render_valid(invalid);
    invalid = effect;
    invalid.angle_degrees = 1;
    invalid_rejected &= !background_effects_valid(invalid) && !background_render_valid(invalid);
    invalid = effect;
    invalid.intensity = 1;
    invalid_rejected &= !background_effects_valid(invalid) && !background_render_valid(invalid);
    invalid = effect;
    invalid.strength = 101;
    invalid_rejected &= !background_effects_valid(invalid) && !background_render_valid(invalid);
    check(invalid_rejected, "flat effects: malformed zero-stop palette, direction, intensity and effect "
                            "parameters fail closed");

    bool gates = !background_animation_allowed(effect, false, false, false, false, true, false) &&
                 !background_animation_allowed(effect, true, true, false, false, true, false) &&
                 !background_animation_allowed(effect, true, false, true, false, true, false) &&
                 !background_animation_allowed(effect, true, false, false, true, true, false) &&
                 !background_animation_allowed(effect, true, false, false, false, false, false) &&
                 !background_animation_allowed(effect, true, false, false, false, true, true);
    for (u32 setting = 0; setting < 3; ++setting) {
        auto paused = effect;
        if (setting == 0)
            paused.speed = 0;
        else if (setting == 1)
            paused.strength = 0;
        else
            paused.density = 0;
        gates = gates && background_render_valid(paused) &&
                !background_animation_allowed(paused, true, false, false, false, true, false);
    }
    check(gates, "flat effects: hidden, covered, accessibility and zero parameter gates admit no animation");

    // ---- Effect pass (mc.fx@1 constants, canvases, selection, GPU guard) ----
    const BackgroundTheme none{};
    check(background_animation_allowed(none, true, true, false, false, false, true, false) &&
              !background_animation_allowed(none, false, true, false, false, false, true, false) &&
              !background_animation_allowed(none, true, true, false, false, false, true, true) &&
              !background_animation_allowed(none, true, true, false, false, true, true, false) &&
              background_animation_allowed(effect, false, true, false, false, false, true, false),
          "effect pass: a plugin effect runs the clock on a theme without one, under the same pause rules");
    PaintStream canvases{};
    bool marked = paint_stream_initialize(&canvases, 8) && paint_stream_reset(&canvases, 200, 100);
    Theme flat = kDefaultTheme;
    Theme starry = kDefaultTheme;
    starry.background = effect;
    marked = marked && paint_stream_push(&canvases, 0, 0, 200, 100, flat.base_lowest) &&
             ui_background_paint_canvas(&canvases, {0, 0, 200, 100}, flat, flat.base_low) &&
             ui_background_paint_canvas(&canvases, {10, 10, 90, 90}, starry, starry.base_low) &&
             canvases.count == 3 && !ui_background_canvas_quad(canvases.quads[0]) &&
             ui_background_canvas_quad(canvases.quads[1]) && canvases.quads[1].border_width == 0 &&
             ui_background_canvas_quad(canvases.quads[2]) && canvases.quads[2].border_width == -8;
    paint_stream_release(&canvases);
    check(marked, "effect pass: every canvas quad is marked on flat and effect themes; plain quads are not");

    // /fp:fast may divide by reciprocal: compare to a few ulps.
    const auto close_to = [](float a, float b) noexcept { return a - b < 1e-5f && b - a < 1e-5f; };
    BackgroundFxConstants constants{};
    Theme light = kLightTheme;
    light.background = effect;
    const bool packed = background_fx_pack_builtin(&constants, starry, 12.5f, {10, 20, 110, 70}, 1.5f, 800, 600);
    bool builtin = packed && constants.time == 12.5f && constants.delta == 0 && constants.dark == 1 &&
                   close_to(constants.intensity, .18f) && constants.resolution[0] == 150 && constants.resolution[1] == 75 &&
                   constants.origin[0] == 15 && constants.origin[1] == 30 && constants.canvas[0] == 10 &&
                   constants.canvas[3] == 70 && constants.parameters[kBackgroundFxMask] == 3 &&
                   close_to(constants.parameters[kBackgroundFxSpeed], .28f) &&
                   close_to(constants.parameters[kBackgroundFxDensity], .22f) &&
                   close_to(constants.parameters[kBackgroundFxStrength], .18f) &&
                   constants.parameters[kBackgroundFxWindowWidth] == 800 &&
                   constants.parameters[kBackgroundFxWindowHeight] == 600 && constants.parameters[6] == 0;
    for (u32 role = 0; role < kBackgroundFxPaletteRoles; ++role)
        builtin = builtin && constants.palette[role][3] == 1;
    builtin = builtin && close_to(constants.palette[0][0], background_fx_linear(starry.base_low.red)) &&
              close_to(constants.palette[4][2], background_fx_linear(starry.accent.blue)) &&
              close_to(constants.palette[7][0], background_fx_linear(starry.tokens.accent.danger.red));
    BackgroundFxConstants light_constants{};
    builtin = builtin && background_fx_pack_builtin(&light_constants, light, 0, {0, 0, 1, 1}, 1, 1, 1) &&
              light_constants.dark == 0 && light_constants.intensity < constants.intensity;
    Theme off = starry;
    off.background.effect = BackgroundEffect::none;
    BackgroundFxConstants rejected{};
    builtin = builtin && !background_fx_pack_builtin(&rejected, off, 0, {0, 0, 1, 1}, 1, 1, 1) &&
              !background_fx_pack(&rejected, starry, 0, 0, {5, 5, 5, 9}, 1) &&
              !background_fx_pack(&rejected, starry, -1, 0, {0, 0, 1, 1}, 1) &&
              background_fx_pack(&rejected, flat, 1, 4, {0, 0, 1, 1}, 1) && rejected.delta == 0 &&
              close_to(rejected.intensity, background_effect_settings_defaults(BackgroundEffect::none).strength / 100.f);
    check(builtin, "effect pass: 192-byte mc.fx constants carry time, theme roles in linear RGB, canvas "
                   "pixels and the Stars/Comets profile; light lowers intensity; empty canvases fail");
    // Stars/Comets draw as sprites: one instance per comet lane, then one per star cell the canvas
    // touches (pitch 151 DIPs at this density), bounded to the window's neighbourhood.
    BackgroundFxConstants grid = constants;
    bool sprites = grid.grid[0] == 0 && grid.grid[3] == 0 && background_fx_builtin_grid(&grid, {10, 20, 110, 70}) == 2 &&
                   grid.grid[0] == 0 && grid.grid[1] == 0 && grid.grid[2] == 1 && grid.grid[3] == 1;
    sprites = sprites && background_fx_builtin_grid(&grid, {0, 0, 800, 600}) == 25 && grid.grid[2] == 6 &&
              background_fx_builtin_grid(&grid, {0, -40, 800, 0}) == 13 && grid.grid[1] == -1 &&
              background_fx_builtin_grid(&grid, {-5000, -5000, -4000, -4000}) == 1 && grid.grid[2] == 0;
    grid.parameters[kBackgroundFxMask] = 1;
    sprites = sprites && background_fx_builtin_grid(&grid, {-5000, -5000, -4000, -4000}) == 0 &&
              background_fx_builtin_grid(&grid, {0, 0, 800, 600}) == 24 && grid.grid[3] == 0;
    grid.parameters[kBackgroundFxMask] = 2;
    grid.parameters[kBackgroundFxDensity] = 1;
    sprites = sprites && background_fx_builtin_grid(&grid, {0, 0, 800, 600}) == 4 && grid.grid[2] == 0;
    check(sprites, "effect pass: built-in sprite grid counts comet lanes and star cells over a canvas, clipped to the window");
    const u32 nan_bits = 0x7fc00000u;
    float nan_value = 0;
    copy_bytes(&nan_value, &nan_bits, sizeof(nan_value));
    check(background_fx_linear(0) == 0 && background_fx_linear(1) > .999f && background_fx_linear(1) < 1.001f &&
              background_fx_linear(.5f) > .21f && background_fx_linear(.5f) < .22f &&
              background_fx_linear(-1) == 0 && background_fx_linear(nan_value) == 0,
          "effect pass: sRGB to linear stays within the curve and clamps NaN and range");

    // ---- Steps and settings: per-shader override > global > the package's request > 50 % at 24 fps ----
    bool steps = effect_resolve_step(0, 0, 0, kEffectDefaultResolutionStep, kEffectResolutionSteps) == 2 &&
                 effect_resolve_step(0, 0, 4, 2, 5) == 3 && effect_resolve_step(0, 5, 4, 2, 5) == 4 &&
                 effect_resolve_step(1, 5, 4, 2, 5) == 0 && effect_resolve_step(9, 0, 0, 2, 5) == 4 &&
                 kEffectResolutionPercent[kEffectDefaultResolutionStep] == 50 &&
                 kEffectFramesPerSecond[kEffectDefaultFrameRateStep] == 24;
    EffectUserSettings user = kEffectUserDefaults;
    EffectShaderSettings own = effect_shader_defaults(0x77);
    EffectSettings resolved = effect_resolve_settings(user, own, 0, 0, EffectBlend::screen);
    steps = steps && resolved.resolution_step == 2 && resolved.frame_rate_step == 2 &&
            resolved.blend == EffectBlend::screen && close_to(resolved.speed, 1.f) && close_to(resolved.opacity, 1.f) &&
            resolved.budget == EffectBudget::balanced && !resolved.pointer_allowed;
    resolved = effect_resolve_settings(user, own, 4, 4, EffectBlend::add);
    steps = steps && resolved.resolution_step == 3 && resolved.frame_rate_step == 3 && resolved.blend == EffectBlend::add;
    user.resolution = 1;
    user.frame_rate = 5;
    resolved = effect_resolve_settings(user, own, 4, 4, EffectBlend::add);
    steps = steps && resolved.resolution_step == 0 && resolved.frame_rate_step == 4;
    own.resolution = 5;
    own.blend = static_cast<u8>(EffectBlend::normal) + 1;
    own.speed = 30;
    own.opacity = 50;
    own.pointer = 1;
    resolved = effect_resolve_settings(user, own, 4, 4, EffectBlend::add);
    steps = steps && resolved.resolution_step == 4 && resolved.frame_rate_step == 4 &&
            resolved.blend == EffectBlend::normal && close_to(resolved.speed, 1.5f) && close_to(resolved.opacity, .5f) &&
            resolved.pointer_allowed && effect_settings_valid(resolved);
    own.opacity = 101;
    steps = steps && !effect_shader_settings_valid(own) && effect_user_settings_valid(user) &&
            !effect_user_settings_valid({6, 0, EffectBudget::low, EffectUnfocused::keep}) &&
            effect_shader_settings_valid(effect_shader_defaults(1)) &&
            !effect_settings_valid({nan_value, 1, EffectBlend::normal, 0, 0, EffectBudget::low, false}) &&
            !effect_settings_valid({1, 1, EffectBlend::normal, 5, 0, EffectBudget::low, false});
    check(steps, "effect pass: steps resolve override over global over request over 50 %/24 fps; settings validate");

    // Frame intervals and the animation clock they drive.
    const auto sample = background_animation_sample(0, 1.f, false, 1000, true, effect_interval_ms(3, false));
    const auto later = background_animation_sample(sample.next_frame_ms, sample.phase_seconds, true, 1100, true,
                                                   effect_interval_ms(3, false));
    // Steps are 10, 15, 24, 30 and 60 fps.
    check(effect_interval_ms(2, false) == 42 && effect_interval_ms(3, false) == 33 && effect_interval_ms(4, false) == 17 &&
              effect_interval_ms(0, false) == 100 && effect_interval_ms(3, true) == 100 &&
              effect_interval_ms(0, true) == 100 && effect_interval_ms(2, true) == 100 && sample.running &&
              sample.next_frame_ms == 1033 && later.next_frame_ms == 1133 && later.phase_seconds > 1.0f &&
              later.phase_seconds <= 1.07f &&
              background_animation_sample(0, 1.f, false, 1000, true).next_frame_ms == 1000 + kBackgroundAnimationIntervalMs,
          "effect pass: frame intervals come from the fps steps (slowed to 10 fps when unfocused) and drive the clock");

    // ---- Budget controller ----
    constexpr float kBalanced = kEffectBudgetMs[1];
    const auto run_samples = [](EffectBudgetState* state, float gpu_ms, float budget, u64* now, u64 until) noexcept {
        EffectBudgetAction action = EffectBudgetAction::none;
        while (action == EffectBudgetAction::none && *now < until) {
            action = effect_budget_sample(state, gpu_ms, budget, *now);
            *now += 40;
        }
        return action;
    };
    EffectBudgetState state{};
    u64 now = 1000;
    effect_budget_start(&state, 9, 4);
    bool controller = state.step == 4 && state.ceiling == 4 && state.warmup == kEffectWarmupSamples;
    // Warm-up samples are free; a second over budget steps one down; a calm spell steps back, never above the ceiling.
    controller = controller && effect_budget_sample(&state, 7, kBalanced, now) == EffectBudgetAction::none &&
                 effect_budget_sample(&state, 7, kBalanced, now) == EffectBudgetAction::none && !state.warmup;
    controller = controller && run_samples(&state, 2.f, kBalanced, &now, now + 3000) == EffectBudgetAction::step_down &&
                 state.step == 3 && state.warmup == kEffectWarmupSamples;
    controller = controller && run_samples(&state, .1f, kBalanced, &now, now + 12000) == EffectBudgetAction::step_up &&
                 state.step == 4;
    controller = controller && run_samples(&state, .1f, kBalanced, &now, now + 12000) == EffectBudgetAction::none &&
                 state.step == 4;
    // Between a third of the budget and the budget: no change either way.
    EffectBudgetState steady{};
    effect_budget_start(&steady, 2, 4);
    now = 1000;
    controller = controller && run_samples(&steady, 1.f, kBalanced, &now, now + 12000) == EffectBudgetAction::none &&
                 steady.step == 2;
    // One stall goes straight to the lowest step; one runaway frame turns the effect off.
    EffectBudgetState stalled{};
    effect_budget_start(&stalled, 3, 3);
    stalled.warmup = 0;
    controller = controller && effect_budget_sample(&stalled, 9, kBalanced, 5) == EffectBudgetAction::drop_to_floor &&
                 stalled.step == 0 && stalled.warmup == kEffectWarmupSamples;
    stalled.warmup = 0;
    controller = controller && effect_budget_sample(&stalled, 9, kBalanced, 6) == EffectBudgetAction::none &&
                 effect_budget_sample(&stalled, 60, kBalanced, 7) == EffectBudgetAction::disable;
    // Over budget at the lowest step for three seconds turns it off; bad samples do nothing.
    EffectBudgetState floor{};
    effect_budget_start(&floor, 0, 4);
    floor.warmup = 0;
    now = 1000;
    controller = controller && run_samples(&floor, 3.f, kBalanced, &now, now + 2500) == EffectBudgetAction::none &&
                 run_samples(&floor, 3.f, kBalanced, &now, now + 3000) == EffectBudgetAction::disable;
    controller = controller && effect_budget_sample(&steady, nan_value, kBalanced, 99999) == EffectBudgetAction::none &&
                 effect_budget_sample(&steady, -1, kBalanced, 99999) == EffectBudgetAction::none &&
                 effect_budget_sample(&steady, 1, 0, 99999) == EffectBudgetAction::none;
    check(controller, "effect pass: budget controller warms up, steps down and up, drops on a stall, disables on a "
                      "runaway or three seconds over at the floor");

    // ---- Selection probe ----
    const EffectProbe linear{.2f, 1.f, 1000, 5000};
    const EffectProbe cheap{.05f, .1f, 4096, 16384};
    const EffectProbe heavy{.5f, 3.f, 4096, 16384};
    bool refuse = true;
    const u32 cheap_low = effect_probe_start_step(cheap, 1920, 1080, kEffectBudgetMs[0], 4, &refuse);
    const bool cheap_refused = refuse;
    const u32 cheap_high = effect_probe_start_step(cheap, 1920, 1080, kEffectBudgetMs[2], 4, &refuse);
    const u32 cheap_capped = effect_probe_start_step(cheap, 1920, 1080, kEffectBudgetMs[2], 1, &refuse);
    const u32 heavy_step = effect_probe_start_step(heavy, 1920, 1080, kEffectBudgetMs[2], 4, &refuse);
    const bool heavy_refused = refuse;
    check(close_to(effect_probe_estimate(linear, 3000), .6f) && close_to(effect_probe_estimate(linear, 100), .2f) &&
              effect_probe_estimate(linear, 9000) > effect_probe_estimate(linear, 3000) && !cheap_refused &&
              cheap_high >= cheap_low && cheap_high <= 4 && cheap_capped <= 1 &&
              (cheap_high == 0 || effect_probe_estimate(cheap, effect_step_pixels(1920, 1080, cheap_high)) <=
                                      kEffectBudgetMs[2]) &&
              heavy_step == 0 && heavy_refused && effect_scaled_extent(1920, 0) == 480 &&
              effect_scaled_extent(1080, 2) == 540 && effect_scaled_extent(1, 0) == 1 &&
              effect_step_pixels(640, 360, 4) == 640u * 360u,
          "effect pass: the cost probe extrapolates linearly, starts below the ceiling and refuses a hopeless effect");

    // ---- Layout of the constants the shaders read ----
    float palette[kBackgroundFxPaletteRoles][4]{};
    effect_palette(starry, palette);
    EffectFrameConstants frame{};
    check(sizeof(frame) == 304 && sizeof(EffectCompositeConstants) == 64 && sizeof(EffectShaderSettings) == 16 &&
              palette[0][0] == starry.base_low.red && palette[4][2] == starry.accent.blue &&
              palette[7][1] == starry.tokens.accent.danger.green && palette[3][3] == 1,
          "effect pass: frame and composite constants keep the shader layout; the palette carries the theme's own sRGB");
}
} // namespace maicord
