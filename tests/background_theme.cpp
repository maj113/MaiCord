#include "maicord/ui/background_theme.hpp"
#include "maicord/ui/background_animation.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/role_color.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/render/content_text_retention.hpp"

namespace maicord {
namespace {
bool near_phase(float a, float b) noexcept {
    return a - b < .0001f && b - a < .0001f;
}
} // namespace
void test_background_theme(void (*check)(bool, const char*) noexcept) noexcept {
    Theme gradient = kDefaultTheme;
    const auto identity = ui_background_canvas_color(gradient, gradient.base_low);
    check(bytes_equal(&identity, &gradient.base_low, sizeof(Color)),
          "background canvas: disabled descriptor retains exact opaque canvas identity");
    gradient.background = background_theme_defaults();
    const Theme original_gradient = gradient;
    const auto dark_tint = ui_background_canvas_color(gradient, gradient.base_low);
    check(near_phase(dark_tint.alpha, .7104f) && dark_tint.red == 0 && dark_tint.green == 0 &&
              dark_tint.blue == 0 && gradient.base_low.alpha == 1 && gradient.text_primary.alpha == 1,
          "background canvas: intensity controls the source-defined black chat plate without changing "
          "tokens");
    Theme light = kLightTheme;
    light.background = gradient.background;
    constexpr u8 intensities[]{0, 10, 20, 40, 60, 74, 80, 100};
    const Theme* modes[]{&gradient, &light};
    for (const Theme* mode : modes) {
        Theme sample = *mode;
        sample.background.color_count = 5;
        sample.background.colors[0] = 0xff0000;
        sample.background.colors[1] = 0x00ff00;
        sample.background.colors[2] = 0x0000ff;
        sample.background.colors[3] = 0xffffff;
        sample.background.colors[4] = 0;
        Color expected[5]{};
        bool admitted = ui_background_palette(sample, expected);
        for (const auto intensity : intensities) {
            sample.background.intensity = intensity;
            Color current[5]{};
            const auto canvas = ui_background_canvas_color(sample, sample.base_low);
            const auto surface = ui_background_surface_color(sample, sample.surface, .2f);
            const auto hover = ui_background_hover_color(sample, sample.surface_high);
            const float multiplier = ((1000u - 7u * intensity + 5u) / 10u) / 100.f;
            const bool is_light = mode == &light;
            const float expected_chat =
                is_light ? .95f * (.6f + .4f * multiplier) : .48f + .32f * multiplier;
            admitted = admitted && ui_background_palette(sample, current) &&
                       bytes_equal(expected, current, sizeof(expected)) &&
                       near_phase(canvas.alpha, expected_chat) && canvas.red == (is_light ? 1.f : 0.f) &&
                       canvas.green == canvas.red && canvas.blue == canvas.red &&
                       near_phase(surface.alpha, .2f) && near_phase(hover.alpha, .2f);
            constexpr UiBackgroundTier tiers[]{UiBackgroundTier::lowest, UiBackgroundTier::lower,
                                               UiBackgroundTier::low,    UiBackgroundTier::high,
                                               UiBackgroundTier::higher, UiBackgroundTier::highest};
            const float dark_intercepts[]{.54f, .51f, .48f, .45f, .42f, .39f};
            const float dark_slopes[]{.36f, .34f, .32f, .30f, .28f, .26f};
            const float light_intercepts[]{.51f, .48f, .45f, .42f, .325f, .8f};
            const float light_slopes[]{.34f, .32f, .30f, .28f, .325f, .2f};
            for (u32 tier = 0; tier < 6; ++tier)
                admitted = admitted &&
                           near_phase(ui_background_tier_opacity(sample, tiers[tier]),
                                      (is_light ? light_intercepts[tier] : dark_intercepts[tier]) +
                                          multiplier * (is_light ? light_slopes[tier] : dark_slopes[tier]));
            for (u32 stop = 0; stop < 5; ++stop) {
                const u32 rgb = sample.background.colors[stop];
                admitted = admitted && current[stop].red == ((rgb >> 16) & 255) / 255.f &&
                           current[stop].green == ((rgb >> 8) & 255) / 255.f &&
                           current[stop].blue == (rgb & 255) / 255.f;
            }
        }
        check(admitted, "background intensity: both modes preserve raw saturated stops and exact rounded "
                        "source tier opacities at every level");
    }
    const Color glass_source{.11f, .22f, .33f, .64f};
    constexpr u8 glass_intensities[]{0, 20, 100};
    bool glass_stable = true;
    for (const auto intensity : glass_intensities) {
        Theme glass_theme = gradient;
        glass_theme.background.intensity = intensity;
        const Theme untouched = glass_theme;
        const auto glass = ui_background_surface_color(glass_theme, glass_source, .3f);
        glass_stable = glass_stable && near_phase(glass.alpha, .192f) && glass.red == glass_source.red &&
                       glass.green == glass_source.green && glass.blue == glass_source.blue &&
                       bytes_equal(&glass_theme, &untouched, sizeof(Theme));
    }
    const auto flat_glass = ui_background_surface_color(kDefaultTheme, glass_source, .3f);
    check(glass_stable && bytes_equal(&flat_glass, &glass_source, sizeof(Color)),
          "background glass: valid0/20/100 retain constant opacity and unchanged RGB/theme; flat remains "
          "byte-exact");
    Theme invalid_theme = gradient;
    invalid_theme.background.color_count = 0;
    Color sentinel[5]{};
    sentinel[0] = {1, 0, 0, 1};
    Color unchanged[5]{};
    copy_bytes(unchanged, sentinel, sizeof(sentinel));
    const auto invalid_surface = ui_background_surface_color(invalid_theme, invalid_theme.surface, .2f);
    check(!ui_background_palette(invalid_theme, sentinel) &&
              bytes_equal(unchanged, sentinel, sizeof(sentinel)) &&
              bytes_equal(&invalid_surface, &invalid_theme.surface, sizeof(Color)),
          "background palette: invalid descriptors leave caller colors and ordinary surfaces atomically "
          "unchanged");
    const auto muted = ui_background_muted_text_color(gradient);
    check(bytes_equal(&muted, &gradient.text_secondary, sizeof(Color)),
          "background foreground: active muted ink uses readable secondary text");
    const auto flat_light_muted = ui_background_muted_text_color(kLightTheme);
    check(bytes_equal(&flat_light_muted, &kLightTheme.text_secondary, sizeof(Color)),
          "background foreground: flat Light muted ink uses semantic secondary text");
    bool custom_secondary = true;
    const Theme* secondary_modes[]{&kDefaultTheme, &kLightTheme, &gradient, &light};
    for (const Theme* mode : secondary_modes) {
        Theme sample = *mode;
        sample.text_secondary = {.13f, .47f, .29f, .61f};
        const Theme untouched = sample;
        const auto ink = ui_background_muted_text_color(sample);
        const auto signature = content_text_theme_signature(sample);
        const bool unchanged_tokens = bytes_equal(&sample, &untouched, sizeof(Theme));
        sample.text_secondary.green = .48f;
        custom_secondary = custom_secondary && unchanged_tokens &&
                           bytes_equal(&ink, &untouched.text_secondary, sizeof(Color)) &&
                           signature != content_text_theme_signature(sample);
    }
    check(custom_secondary,
          "background foreground: flat and active Dark/Light preserve exact custom secondary RGBA "
          "and invalidate retained text when that token changes");
    Theme recolored = original_gradient;
    recolored.background.colors[0] ^= 0xffffff;
    check(content_text_theme_signature(recolored) == content_text_theme_signature(original_gradient),
          "background retention: changing only background preserves foreground text signature");
    recolored.text_primary.red = .1f;
    check(content_text_theme_signature(recolored) != content_text_theme_signature(original_gradient),
          "background retention: foreground change invalidates text signature");
    const Color roles[]{{1, 0, 0, .72f}, {0, 1, 0, .72f}, {0, 0, 1, .72f}, {.7f, .2f, .85f, .72f}};
    bool role_policy = true;
    for (const auto role : roles) {
        const auto normal = ui_foreground_color(kDefaultTheme, role);
        const auto adapted_dark = ui_foreground_color(original_gradient, role);
        const auto adapted_light = ui_foreground_color(kLightTheme, role);
        role_policy = role_policy && bytes_equal(&normal, &role, sizeof(role)) &&
                      adapted_dark.alpha == role.alpha && adapted_light.alpha == role.alpha &&
                      ui_background_luminance(adapted_dark) >= .49999f &&
                      ui_background_luminance(adapted_light) <= .10001f;
        const Color adapted_colors[]{adapted_dark, adapted_light};
        for (const auto adapted : adapted_colors) {
            // Mixing toward black/white scales pairwise channel differences
            // uniformly, retaining semantic hue rather than choosing gray ink.
            const float hue_error = (adapted.red - adapted.green) * (role.red - role.blue) -
                                    (adapted.red - adapted.blue) * (role.red - role.green);
            role_policy = role_policy && hue_error > -.0001f && hue_error < .0001f &&
                          (role.red > role.green ? adapted.red > adapted.green : true) &&
                          (role.green > role.blue ? adapted.green > adapted.blue : true) &&
                          (role.blue > role.red ? adapted.blue > adapted.red : true);
        }
        Theme draft = original_gradient;
        draft.background.colors[0] ^= 0xffffff;
        draft.background.angle_degrees = 271;
        draft.background.intensity = 80;
        draft.background.effect = BackgroundEffect::comets;
        draft.background.speed = 17;
        draft.background.strength = 64;
        draft.background.density = 88;
        const auto recolored_role = ui_foreground_color(draft, role);
        role_policy = role_policy && bytes_equal(&adapted_dark, &recolored_role, sizeof(Color));
        draft.background = {};
        const auto inactive_role = ui_foreground_color(draft, role);
        role_policy = role_policy && bytes_equal(&inactive_role, &role, sizeof(Color));
    }
    check(role_policy, "background foreground: semantic token hue and alpha survive mode contrast "
                       "adaptation; flat Dark is exact and palette/effect changes do not alter token ink");
    // Server role colours are data, never semantic tokens: every theme type
    // (flat dark, flat light, dark gradient, light gradient) shows them
    // exactly; enhanced role styles only add the official light brightness(.85).
    Theme light_gradient = kLightTheme;
    light_gradient.background = original_gradient.background;
    const Theme role_themes[]{kDefaultTheme, kLightTheme, original_gradient, light_gradient};
    RoleRecord flat_role{};
    flat_role.color = 0x9B59B6u;
    RoleRecord gradient_role = flat_role;
    gradient_role.secondary_color = 0x1ABC9Cu | kRoleColorPresent;
    RoleRecord holographic_role = gradient_role;
    holographic_role.tertiary_color = 0x000000u | kRoleColorPresent;
    const auto exact = ui_role_color(0x9B59B6u);
    bool role_data = exact.red == 0x9B / 255.f && exact.green == 0x59 / 255.f && exact.blue == 0xB6 / 255.f &&
                     exact.alpha == 1;
    for (const auto& theme : role_themes) {
        const bool light_theme = theme.appearance == ThemeAppearance::light;
        const auto flat = ui_role_gradient_effect(theme, flat_role);
        const auto two = ui_role_gradient_effect(theme, gradient_role);
        const auto three = ui_role_gradient_effect(theme, holographic_role);
        const u32 primary = light_theme ? (0x9Bu * 85 / 100 << 16 | 0x59u * 85 / 100 << 8 | 0xB6u * 85 / 100)
                                        : 0x9B59B6u;
        const u32 secondary = light_theme ? (0x1Au * 85 / 100 << 16 | 0xBCu * 85 / 100 << 8 | 0x9Cu * 85 / 100)
                                          : 0x1ABC9Cu;
        role_data = role_data && !flat.kind && two.kind == kUiTextEffectRoleGradient && two.count == 2 &&
                    two.colors[0] == primary && two.colors[1] == secondary && !two.animate &&
                    three.count == 3 && three.colors[2] == 0 && three.colors[1] == secondary;
    }
    check(role_data, "role colours: flat, gradient and holographic role colours pass through every theme type "
                     "unchanged (light themes dim only enhanced gradients by .85)");
    auto contrast_preferences = client_preferences_defaults();
    contrast_preferences.theme = ClientTheme::custom_gradient;
    const auto dark_foreground = client_theme_projection(contrast_preferences);
    contrast_preferences.theme = ClientTheme::custom_light_gradient;
    const auto light_foreground = client_theme_projection(contrast_preferences);
    const auto dark_muted = ui_background_muted_text_color(dark_foreground);
    const auto light_muted = ui_background_muted_text_color(light_foreground);
    check(dark_foreground.text_primary.alpha == 1 && light_foreground.text_primary.alpha == 1 &&
              ui_background_luminance(dark_muted) > .25f && ui_background_luminance(light_muted) < .3f,
          "background foreground: source-tinted primary and secondary inks stay opaque in both modes");
    contrast_preferences.theme = ClientTheme::custom_gradient;
    contrast_preferences.gradient.colors[0] ^= 0xffffff;
    contrast_preferences.gradient.intensity = 80;
    const auto changed_palette = client_theme_projection(contrast_preferences);
    const auto changed_signature = content_text_theme_signature(changed_palette);
    contrast_preferences.gradient.angle_degrees = 271;
    contrast_preferences.gradient.effect = BackgroundEffect::comets;
    contrast_preferences.gradient.density = 99;
    const auto changed_motion = client_theme_projection(contrast_preferences);
    check(changed_signature != content_text_theme_signature(dark_foreground) &&
              content_text_theme_signature(changed_motion) == changed_signature &&
              content_text_theme_signature(light_foreground) !=
                  content_text_theme_signature(dark_foreground),
          "background foreground retention: actual projected color changes invalidate; direction and "
          "effect-only changes retain");
    Theme raw_active = kDefaultTheme;
    raw_active.background = background_theme_defaults();
    Theme raw_zero = raw_active;
    raw_zero.background.intensity = 0;
    Theme raw_recolored = raw_active;
    raw_recolored.background.colors[0] ^= 0xffffff;
    raw_recolored.background.intensity = 80;
    Theme raw_light_active = kLightTheme;
    raw_light_active.background = raw_active.background;
    check(
        content_text_theme_signature(raw_active) != content_text_theme_signature(kDefaultTheme) &&
            content_text_theme_signature(raw_zero) == content_text_theme_signature(raw_active) &&
            content_text_theme_signature(raw_recolored) == content_text_theme_signature(raw_active) &&
            content_text_theme_signature(raw_light_active) != content_text_theme_signature(kLightTheme),
        "background foreground retention: raw activity invalidates authored-role ink policy "
        "even with "
        "identical tokens; valid zero intensity and palette changes reuse identical raw foreground tokens");
    PaintStream materials{};
    check(paint_stream_initialize(&materials) && paint_stream_reset(&materials, 240, 100),
          "background controls: initialize bounded paint stream with active viewport clip");
    const UiRect material_rect{0, 0, 240, 100};
    auto palette = background_theme_defaults();
    palette.colors[3] = 0xaa4422;
    palette.colors[4] = 0xaabbcc;
    palette.color_count = 5;
    palette.angle_degrees = 90;
    bool encoded =
        paint_stream_push_palette_gradient(&materials, material_rect, palette, 8) && materials.count == 4;
    for (u32 i = 0; i < materials.count; ++i)
        encoded = encoded && materials.quads[i].border_width == -6 &&
                  near_phase(materials.quads[i].fill.alpha, i * .25f) &&
                  near_phase(materials.quads[i].border.alpha, (i + 1) * .25f) &&
                  near_phase(materials.quads[i].reserved_x, 1) &&
                  near_phase(materials.quads[i].reserved_y, 0);
    check(encoded, "background controls: five stop directional preview stores exact interval metadata "
                   "without texture");
    const u32 old_count = materials.count, old_batches = materials.batch_count;
    palette.color_count = 6;
    check(!paint_stream_push_palette_gradient(&materials, material_rect, palette, 8) &&
              materials.count == old_count && materials.batch_count == old_batches,
          "background controls: invalid palette leaves stream atomically unchanged");
    check(paint_stream_push_sv_square(&materials, material_rect, {1, 0, 0, 1}, 8) &&
              paint_stream_push_hue_strip(&materials, material_rect, 8) &&
              materials.count == old_count + 2 && !materials.rejected &&
              materials.quads[old_count].border_width == -4 &&
              materials.quads[old_count + 1].border_width == -5,
          "background controls: SV and hue use native bounded paint materials");
    const u32 surface_first = materials.count;
    const Color surface_fill{.16f, .17f, .19f, 1}, surface_border{.2f, .3f, .4f, 1};
    bool surface_material =
        ui_background_paint_surface(&materials, material_rect, gradient, surface_fill, surface_border, 8, 2,
                                    .9f, UiBackgroundSurfaceMode::gradient);
    surface_material =
        surface_material && materials.count == surface_first + 1 &&
        materials.quads[surface_first].border_width == -7 &&
        materials.quads[surface_first].reserved_x == 2 &&
        near_phase(materials.quads[surface_first].reserved_y,
                   (1 - ui_background_tier_opacity(gradient, UiBackgroundTier::highest)) * .9f) &&
        bytes_equal(&materials.quads[surface_first].fill, &surface_fill, sizeof(Color)) &&
        bytes_equal(&materials.quads[surface_first].border, &surface_border, sizeof(Color));
    check(surface_material, "background surface: one native material preserves opaque fill and original "
                            "rounded border metadata");
    const u32 tinted_first = materials.count;
    check(ui_background_paint_surface(&materials, material_rect, gradient, surface_fill, surface_border, 8,
                                      2) &&
              paint_stream_push_rounded_border(&materials, 0, 0, 240, 100, surface_fill, surface_border, 8,
                                               2) &&
              bytes_equal(&materials.quads[tinted_first], &materials.quads[tinted_first + 1],
                          sizeof(PaintQuad)),
          "background surface: tinted default keeps source-projected static opaque tokens byte-exact");
    const u32 palette_edge_first = materials.count;
    check(ui_background_paint_surface(&materials, material_rect, gradient, surface_fill, surface_border, 8,
                                      2, 1, UiBackgroundSurfaceMode::gradient, UiBackgroundTier::chat,
                                      UiBackgroundBorderTone::palette) &&
              materials.quads[palette_edge_first].reserved_x == -2 &&
              near_phase(materials.quads[palette_edge_first].reserved_y,
                         1 - ui_background_tier_opacity(gradient, UiBackgroundTier::chat)),
          "background surface: explicit tier and palette edge share one bounded material without "
          "semantic-border mutation");
    const u32 flat_first = materials.count;
    check(
        ui_background_paint_surface(&materials, material_rect, kDefaultTheme, surface_fill, surface_border,
                                    8, 2) &&
            paint_stream_push_rounded_border(&materials, 0, 0, 240, 100, surface_fill, surface_border, 8,
                                             2) &&
            bytes_equal(&materials.quads[flat_first], &materials.quads[flat_first + 1], sizeof(PaintQuad)),
        "background surface: disabled theme is byte-exact ordinary rounded-border geometry");
    const u32 before_invalid = materials.count;
    check(!ui_background_paint_surface(&materials, material_rect, gradient, surface_fill, surface_border, 8,
                                       2, 1.1f) &&
              !ui_background_paint_surface(&materials, material_rect, gradient, surface_fill,
                                           surface_border, 8, 2, .9f,
                                           static_cast<UiBackgroundSurfaceMode>(2)) &&
              materials.count == before_invalid,
          "background surface: invalid palette share or mode rejects before mutating geometry");
    Theme flat_effect = kDefaultTheme;
    flat_effect.background = {};
    flat_effect.background.effect = BackgroundEffect::stars;
    const auto star_defaults = background_effect_settings_defaults(BackgroundEffect::stars);
    flat_effect.background.speed = star_defaults.speed;
    flat_effect.background.strength = star_defaults.strength;
    flat_effect.background.density = star_defaults.density;
    const u32 canvas_first = materials.count;
    const auto flat_text_key = content_text_theme_signature(flat_effect);
    bool flat_canvas =
        background_effects_valid(flat_effect.background) &&
        background_render_valid(flat_effect.background) &&
        background_effects_active(flat_effect.background) &&
        !background_theme_valid(flat_effect.background) && !ui_background_active(flat_effect) &&
        flat_text_key == content_text_theme_signature(kDefaultTheme) &&
        ui_background_paint_canvas(&materials, material_rect, flat_effect, surface_fill) &&
        materials.quads[canvas_first].border_width == -8 && materials.quads[canvas_first].reserved_y == 0 &&
        bytes_equal(&materials.quads[canvas_first].fill, &surface_fill, sizeof(Color));
    const u32 flat_surface_first = materials.count;
    flat_canvas = flat_canvas &&
                  ui_background_paint_surface(&materials, material_rect, flat_effect, surface_fill,
                                              surface_border, 8, 2, 1, UiBackgroundSurfaceMode::gradient) &&
                  materials.quads[flat_surface_first].border_width == 2;
    check(flat_canvas, "background flat effects: independent canvas admission preserves original fill, "
                       "ordinary controls, gradient inactivity and retained foreground signature");
    flat_effect.background.speed = 0;
    check(background_effects_active(flat_effect.background) &&
              !background_animation_allowed(flat_effect.background, true, false, false, false, true, false),
          "background flat effects: zero speed freezes visible artwork without an animation deadline");
    auto malformed_flat = flat_effect.background;
    malformed_flat.colors[4] = 1;
    check(!background_render_valid(malformed_flat),
          "background flat effects: dormant palette bytes cannot enter effect-only rendering");
    paint_stream_release(&materials);
    auto value = background_theme_defaults();
    check(sizeof(BackgroundTheme) == 28 && background_theme_valid(value),
          "background theme: compact default descriptor is valid without retained resources");
    const Utf8View palettes[]{utf8_literal("#123456"), utf8_literal("#000000,#FFFFFF"),
                              utf8_literal(" 6935b8, #164D80,\t16836f "),
                              utf8_literal("#010203,#040506,#070809,#0A0B0C"),
                              utf8_literal("#000000,#123456,#789abc,#DEF012,#ffffff")};
    for (u32 i = 0; i < 5; ++i) {
        value = background_theme_defaults();
        value.angle_degrees = 271;
        value.effect = BackgroundEffect::stars_and_comets;
        value.speed = 77;
        value.intensity = 63;
        bool parsed = background_theme_parse_colors(palettes[i], &value) && value.color_count == i + 1 &&
                      background_theme_valid(value) && value.angle_degrees == 271 &&
                      value.effect == BackgroundEffect::stars_and_comets && value.speed == 77 &&
                      value.intensity == 63;
        for (u32 stop = value.color_count; stop < 5; ++stop)
            parsed = parsed && !value.colors[stop];
        check(parsed,
              "background theme: one to five RGB stops parse with spaces and preserve effect settings");
    }
    const auto retained = value;
    const Utf8View rejected[]{utf8_literal(""),
                              utf8_literal("#12345"),
                              utf8_literal("#12345,#abcdef"),
                              utf8_literal("#1234567,#abcdef"),
                              utf8_literal("#gg0000,#ffffff"),
                              utf8_literal("#000000;#ffffff"),
                              utf8_literal("#000000,#ffffff,"),
                              utf8_literal("#000000,#ffffff,   "),
                              utf8_literal("#000000,#ffffff\n"),
                              utf8_literal("#000000,#111111,#222222,#333333,#444444,#555555")};
    bool atomic = true;
    for (const auto text : rejected)
        atomic = atomic && !background_theme_parse_colors(text, &value) &&
                 background_theme_equal(value, retained);
    atomic = atomic && !background_theme_parse_colors({}, &value) &&
             !background_theme_parse_colors(palettes[0], nullptr) &&
             background_theme_equal(value, retained);
    check(atomic,
          "background theme: malformed or excess stops reject atomically preserving complete descriptor");
    constexpr u16 cardinal_angles[]{0, 90, 180, 270};
    constexpr float cardinal_x[]{0, 1, 0, -1}, cardinal_y[]{-1, 0, 1, 0};
    bool css_directions = true;
    for (u32 i = 0; i < 4; ++i) {
        float x = 0, y = 0;
        css_directions = css_directions && ui_background_direction(cardinal_angles[i], &x, &y) &&
                         near_phase(x, cardinal_x[i]) && near_phase(y, cardinal_y[i]);
    }
    check(css_directions, "background direction: CSS zero-up clockwise cardinal directions");
    const u16 angles[]{0, 1, 45, 120, 271, 359, 360};
    for (const auto angle : angles) {
        value = retained;
        value.angle_degrees = angle;
        check(background_theme_valid(value),
              "background theme: arbitrary bounded gradient angle is admitted");
    }
    bool invalid = true;
    for (u32 field = 0; field < 10; ++field) {
        value = background_theme_defaults();
        switch (field) {
        case 0:
            value.angle_degrees = 361;
            break;
        case 1:
            value.color_count = 0;
            break;
        case 2:
            value.color_count = 6;
            break;
        case 3:
            value.intensity = 101;
            break;
        case 4:
            value.speed = 101;
            break;
        case 5:
            value.effect = static_cast<BackgroundEffect>(kBackgroundEffectCount);
            break;
        case 6:
            value.colors[0] = 0x1000000;
            break;
        case 7:
            value.colors[4] = 1;
            break;
        case 8:
            value.strength = 101;
            break;
        default:
            value.density = 101;
            break;
        }
        invalid = invalid && !background_theme_valid(value);
    }
    check(invalid,
          "background theme: invalid angles effects RGB bounds strength density and inactive stops reject");
    auto preferences = client_preferences_defaults();
    preferences.gradient = retained;
    const ClientTheme themes[]{ClientTheme::dark,   ClientTheme::amoled,
                               ClientTheme::aurora, ClientTheme::sunset,
                               ClientTheme::ocean,  ClientTheme::custom_gradient,
                               ClientTheme::light,  ClientTheme::custom_light_gradient};
    bool projected = true;
    for (const auto selected : themes) {
        preferences.theme = selected;
        const auto theme = client_theme_projection(preferences);
        const auto baseline =
            selected == ClientTheme::light || selected == ClientTheme::custom_light_gradient ? kLightTheme
            : selected == ClientTheme::amoled                                                ? kAmoledTheme
                                              : kDefaultTheme;
        projected = projected && theme.text_primary.alpha == 1 && theme.text_secondary.alpha == 1 &&
                    theme.surface.alpha == 1 && theme.interactive.alpha == 1 &&
                    bytes_equal(&theme.accent, &baseline.accent, sizeof(Color)) &&
                    bytes_equal(&theme.notification, &baseline.notification, sizeof(Color));
        if (selected >= ClientTheme::aurora && selected != ClientTheme::light) {
            projected = projected && background_theme_valid(theme.background) &&
                        theme.background.angle_degrees == retained.angle_degrees &&
                        theme.background.effect == retained.effect &&
                        theme.background.speed == retained.speed &&
                        theme.background.intensity == retained.intensity;
        } else {
            projected = projected && background_effects_valid(theme.background) &&
                        !background_theme_valid(theme.background) &&
                        theme.background.effect == retained.effect &&
                        theme.background.speed == retained.speed &&
                        theme.background.strength == retained.strength &&
                        theme.background.density == retained.density &&
                        bytes_equal(&theme, &baseline, 20 * sizeof(Color));
        }
        if (selected == ClientTheme::custom_gradient || selected == ClientTheme::custom_light_gradient)
            projected = projected && background_theme_equal(theme.background, retained);
        if (selected == ClientTheme::aurora)
            projected = projected && theme.background.color_count == 3 &&
                        theme.background.colors[0] == 0x6935b8 && theme.background.colors[1] == 0x164d80 &&
                        theme.background.colors[2] == 0x16836f && !theme.background.colors[3] &&
                        !theme.background.colors[4];
    }
    check(projected, "background theme: presets project source-tinted opaque surfaces and text while "
                     "preserving semantic accent tokens");
    preferences.theme = ClientTheme::custom_gradient;
    preferences.gradient.strength = 101;
    check(background_theme_equal(client_theme_projection(preferences).background,
                                 background_theme_defaults()),
          "background theme: invalid custom descriptor projects safe default without mutating preferences");
    const auto both_profile = background_effect_settings_defaults(BackgroundEffect::stars_and_comets);
    auto both = background_theme_defaults();
    both.effect = BackgroundEffect::stars_and_comets;
    both.speed = both_profile.speed;
    both.strength = both_profile.strength;
    both.density = both_profile.density;
    check(kBackgroundEffectCount == 4 && kBackgroundEffectProfileCount == 3 && sizeof(both) == 28 &&
              sizeof(both_profile) == 3 && background_theme_valid(both) &&
              background_effect_profile_index(both.effect) == 2 &&
              background_effect_settings_valid(both_profile) && both_profile.speed == 28 &&
              both_profile.strength == 18 && both_profile.density == 22 &&
              background_animation_allowed(both, true, false, false, false, true, false),
          "background effect: catalog is None, Stars, Comets and Stars and comets with compact profiles");
    auto animated = background_theme_defaults();
    animated.effect = BackgroundEffect::stars;
    check(background_animation_allowed(animated, true, false, false, false, true, false),
          "background animation: visible enabled stars admit bounded animation");
    bool blocked = true;
    for (u32 reason = 0; reason < 12; ++reason) {
        value = animated;
        if (reason == 0)
            value.effect = BackgroundEffect::none;
        if (reason == 1)
            value.speed = 0;
        if (reason == 8)
            value.strength = 101;
        if (reason == 9)
            value.color_count = 0;
        if (reason == 10)
            value.strength = 0;
        if (reason == 11)
            value.density = 0;
        blocked = blocked && !background_animation_allowed(value, reason != 2, reason == 3, reason == 4,
                                                           reason == 5, reason != 6, reason == 7);
    }
    check(blocked,
          "background animation: no effect zero speed hidden minimized resize reduced "
          "system-disabled covered invalid descriptors and zero-strength/density have no animation");
    animated.intensity = 0;
    check(background_animation_allowed(animated, true, false, false, false, true, false),
          "background animation: valid zero-intensity weak gradient still permits enabled effects");
    auto sample = background_animation_sample(0, 2.f, false, 10000, true);
    bool stable = sample.running && sample.next_frame_ms == 10042 && sample.phase_seconds == 2.f;
    for (u64 now = 10001; now < 10042; ++now) {
        sample = background_animation_sample(sample.next_frame_ms, sample.phase_seconds, sample.running,
                                             now, true);
        stable = stable && sample.next_frame_ms == 10042 && sample.phase_seconds == 2.f;
    }
    check(
        stable,
        "background animation: repeated warm samples retain exact absolute deadline without postponement");
    sample = background_animation_sample(sample.next_frame_ms, sample.phase_seconds, sample.running, 10042,
                                         true);
    check(sample.next_frame_ms == 10084 && near_phase(sample.phase_seconds, 2.042f),
          "background animation: due sample advances one 42ms phase and arms next deadline");
    sample = background_animation_sample(sample.next_frame_ms, sample.phase_seconds, sample.running,
                                         1000000, true);
    check(sample.next_frame_ms == 1000042 && near_phase(sample.phase_seconds, 2.126f),
          "background animation: long scheduling delay caps phase advance to 84ms with no catch-up loop");
    const float before_pause = sample.phase_seconds;
    sample = background_animation_sample(sample.next_frame_ms, sample.phase_seconds, sample.running,
                                         1000010, false);
    check(!sample.running && !sample.next_frame_ms && sample.phase_seconds == before_pause,
          "background animation: pause retires deadline and freezes phase");
    sample = background_animation_sample(sample.next_frame_ms, sample.phase_seconds, sample.running,
                                         5000000, true);
    check(sample.running && sample.next_frame_ms == 5000042 && sample.phase_seconds == before_pause,
          "background animation: resume rebases deadline without integrating hidden elapsed time");
}
} // namespace maicord
