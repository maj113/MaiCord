#include "maicord/app/client_theme.hpp"
#include "maicord/app/background_frame_policy.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/content_text_retention.hpp"
#include "maicord/render/background_fx.hpp"
#include "maicord/ui/background_animation.hpp"
#include "maicord/ui/background_surface.hpp"

namespace maicord {
void test_background_effect_policy(void (*check)(bool, const char*) noexcept) noexcept {
    static_assert(sizeof(Theme) <= 2048); // room for the ANSI code block palette tokens
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
    const u32 nan_bits = 0x7fc00000u;
    float nan_value = 0;
    copy_bytes(&nan_value, &nan_bits, sizeof(nan_value));
    check(background_fx_linear(0) == 0 && background_fx_linear(1) > .999f && background_fx_linear(1) < 1.001f &&
              background_fx_linear(.5f) > .21f && background_fx_linear(.5f) < .22f &&
              background_fx_linear(-1) == 0 && background_fx_linear(nan_value) == 0,
          "effect pass: sRGB to linear stays within the curve and clamps NaN and range");

    BackgroundFxGuard guard{};
    bool average = true;
    for (u32 frame = 0; frame < 60; ++frame)
        average = average && !background_fx_guard_sample(&guard, 1.9f, 1000 + frame * 16);
    average = average && !background_fx_guard_sample(&guard, 1.9f, 2000);
    BackgroundFxGuard slow{};
    bool tripped = false;
    for (u32 frame = 0; frame <= 62 && !tripped; ++frame)
        tripped = background_fx_guard_sample(&slow, 2.5f, 1000 + frame * 17);
    BackgroundFxGuard stalls{};
    const bool first_stalls = !background_fx_guard_sample(&stalls, 9, 10) && !background_fx_guard_sample(&stalls, 1, 20) &&
                              !background_fx_guard_sample(&stalls, 8.5f, 30);
    const bool third_stall = background_fx_guard_sample(&stalls, 20, 40);
    BackgroundFxGuard nan{};
    check(average && tripped && slow.window_frames == 0 && first_stalls && third_stall &&
              !background_fx_guard_sample(&nan, nan_value, 5),
          "effect pass: GPU guard trips above 2 ms average over a second or on the third 8 ms frame");

    u8 record[kBackgroundFxChoiceBytes]{};
    background_fx_choice_encode({0x1122334455667788ull, 9}, record);
    BackgroundFxChoice decoded{};
    bool choice = background_fx_choice_decode(record, sizeof(record), &decoded) &&
                  decoded.owner_id == 0x1122334455667788ull && decoded.effect_id == 9;
    record[4] = 2;
    choice = choice && !background_fx_choice_decode(record, sizeof(record), &decoded) && !decoded.owner_id;
    background_fx_choice_encode({1, 0}, record);
    choice = choice && !background_fx_choice_decode(record, sizeof(record), &decoded) &&
             !background_fx_choice_decode(record, sizeof(record) - 1, &decoded);
    choice = choice &&
             background_fx_choice_before(utf8_literal("aurora"), {9, 1}, utf8_literal("Borealis"), {1, 1}) &&
             !background_fx_choice_before(utf8_literal("Borealis"), {1, 1}, utf8_literal("aurora"), {9, 1}) &&
             background_fx_choice_before(utf8_literal("Rain"), {1, 1}, utf8_literal("rain"), {2, 1}) &&
             background_fx_choice_before(utf8_literal("Rain"), {1, 1}, utf8_literal("Rainbow"), {1, 2});
    check(choice, "effect pass: saved plugin effect choice round-trips owner+effect, rejects other versions "
                  "and ids, and the picker orders effects by name");
}
} // namespace maicord
