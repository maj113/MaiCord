#pragma once
#include "maicord/ui/theme/theme.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
inline bool ui_background_direction(u16 degrees, float* x, float* y) noexcept {
    if (degrees > 360)
        return false;
    float angle = degrees * (3.14159265358979323846f / 180.f);
    if (angle > 3.14159265358979323846f)
        angle -= 6.28318530717958647692f;
    float sign = 1;
    if (angle > 1.57079632679489661923f) {
        angle = 3.14159265358979323846f - angle;
        sign = -1;
    } else if (angle < -1.57079632679489661923f) {
        angle = -3.14159265358979323846f - angle;
        sign = -1;
    }
    // Minimax polynomials on [-pi/2, pi/2]: cosine within 7.9e-6, sine within 8.7e-7.
    const float squared = angle * angle;
    const float cosine = sign * (1 + squared * (-.499935634f + squared * (.041507071f + squared * -.00127575314f)));
    const float sine = angle * (1 + squared * (-.166656905f + squared * (.0083124612f + squared * -.000184944074f)));
    // CSS linear-gradient: zero points up, positive angles rotate clockwise.
    *x = sine;
    *y = -cosine;
    return true;
}
inline float ui_background_luminance(Color color) noexcept {
    return .2126f * color.red * color.red + .7152f * color.green * color.green +
           .0722f * color.blue * color.blue;
}
enum class UiBackgroundTier : u8 {
    chat,
    lowest,
    lower,
    low,
    high,
    higher,
    highest,
    app_frame = higher
};
inline bool ui_background_active(const Theme& theme) noexcept {
    return background_theme_valid(theme.background);
}
// Tier opacities keep raw stop saturation. Intensity varies the
// black/white canvas layer instead of rescaling the color stops. Only for a
// theme whose background is active (ui_background_active): every caller has
// checked it.
inline float ui_background_tier_opacity(const Theme& theme, UiBackgroundTier tier) noexcept {
    // Each tier's dark and light opacity: scale * (base + slope * multiplier). Only the light chat tier scales.
    static constexpr float kTierOpacity[7][2][3]{
        {{.48f, .32f, 1}, {.6f, .4f, .95f}}, {{.54f, .36f, 1}, {.51f, .34f, 1}},   {{.51f, .34f, 1}, {.48f, .32f, 1}},
        {{.48f, .32f, 1}, {.45f, .30f, 1}},  {{.45f, .30f, 1}, {.42f, .28f, 1}},   {{.42f, .28f, 1}, {.325f, .325f, 1}},
        {{.39f, .26f, 1}, {.8f, .2f, 1}}};
    static_assert(static_cast<u32>(UiBackgroundTier::highest) == 6);
    // The intensity multiplier is rounded before deriving layer opacities.
    const float multiplier = ((1000u - 7u * theme.background.intensity + 5u) / 10u) / 100.f;
    const float* row = kTierOpacity[static_cast<u32>(tier)][theme.appearance == ThemeAppearance::light];
    return row[2] * (row[0] + row[1] * multiplier);
}
inline bool ui_background_palette(const Theme& theme, Color* colors) noexcept {
    if (!colors || !ui_background_active(theme))
        return false;
    for (u32 i = 0; i < theme.background.color_count; ++i) {
        colors[i] = color_from_rgb24(theme.background.colors[i]);
    }
    for (u32 i = theme.background.color_count; i < 5; ++i)
        reset_value(&colors[i]);
    return true;
}
inline Color ui_background_canvas_color(const Theme& theme, Color plate) noexcept {
    if (!ui_background_active(theme))
        return plate;
    const float mode = theme.appearance == ThemeAppearance::light ? 1.f : 0.f;
    return {mode, mode, mode, plate.alpha * ui_background_tier_opacity(theme, UiBackgroundTier::chat)};
}
inline Color ui_background_surface_color(const Theme& theme, const Color& color, float opacity) noexcept {
    Color result = color;
    if (!ui_background_active(theme))
        return result;
    if (!(opacity >= 0))
        opacity = 0;
    result.alpha *= minimum(opacity, 1.f);
    return result;
}
// Hover adds one glass highlight; it must not add an intensity-dependent
// opaque canvas over the color underneath. The glass keeps the tone of the
// (neutral) color it is given but takes the hue of state.tint, shaded toward
// black or tinted toward white, so it reads as the theme rather than gray.
inline Color ui_background_hover_color(const Theme& theme, const Color& color, float opacity = .20f) noexcept {
    Color result = color;
    if (!ui_background_active(theme))
        return result;
    const Color hue = theme.tokens.state.tint;
    const auto tone = [](const Color& c) noexcept { return .2126f * c.red + .7152f * c.green + .0722f * c.blue; };
    const float want = tone(color), have = tone(hue);
    const float shade = want <= have ? (have > 0 ? want / have : 0) : 1;
    const float lift = want > have ? (want - have) / (1 - have) : 0;
    result.red = hue.red * shade + (1 - hue.red) * lift;
    result.green = hue.green * shade + (1 - hue.green) * lift;
    result.blue = hue.blue * shade + (1 - hue.blue) * lift;
    result.alpha *= opacity >= 0 ? minimum(opacity, 1.f) : 0;
    return result;
}
// Thin connector lines (reply arms, thread spines, a forward's edge): their
// usual tone in the theme's tint. Flat themes keep the colour.
inline Color ui_background_line_color(const Theme& theme) noexcept {
    return ui_background_hover_color(theme, theme.interactive_hover, 1);
}
// Hairline separators (a rule between a header and its content, between list
// rows, in a rail or a menu): the subtle border token on flat themes, and on a
// background theme the reply spine's tint so the line reads against the
// gradient. Every separator takes its colour from here.
inline Color ui_separator_color(const Theme& theme) noexcept {
    return ui_background_active(theme) ? ui_background_line_color(theme) : theme.tokens.border.subtle;
}
inline Color ui_background_muted_text_color(const Theme& theme) noexcept {
    return theme.text_secondary;
}
} // namespace maicord
