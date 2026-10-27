#pragma once
#include "maicord/ui/theme.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
inline bool ui_background_direction(u16 degrees, float* x, float* y) noexcept {
    if (!x || !y || degrees > 360)
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
    const float squared = angle * angle;
    const float cosine =
        sign *
        (1 + squared * (-.5f + squared * (1.f / 24 + squared * (-1.f / 720 + squared * (1.f / 40320)))));
    const float sine =
        angle * (1 + squared * (-1.f / 6 + squared * (1.f / 120 +
                                                      squared * (-1.f / 5040 + squared * (1.f / 362880)))));
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
// black/white canvas layer instead of rescaling the color stops.
inline float ui_background_tier_opacity(const Theme& theme, UiBackgroundTier tier) noexcept {
    if (!ui_background_active(theme))
        return 1;
    // The intensity multiplier is rounded before deriving layer opacities.
    const float multiplier = ((1000u - 7u * theme.background.intensity + 5u) / 10u) / 100.f;
    const bool light = theme.appearance == ThemeAppearance::light;
    if (tier == UiBackgroundTier::chat)
        return light ? .95f * (.6f + .4f * multiplier) : .48f + .32f * multiplier;
    switch (tier) {
    case UiBackgroundTier::lowest:
        return light ? .51f + .34f * multiplier : .54f + .36f * multiplier;
    case UiBackgroundTier::lower:
        return light ? .48f + .32f * multiplier : .51f + .34f * multiplier;
    case UiBackgroundTier::low:
        return light ? .45f + .30f * multiplier : .48f + .32f * multiplier;
    case UiBackgroundTier::high:
        return light ? .42f + .28f * multiplier : .45f + .30f * multiplier;
    case UiBackgroundTier::higher:
        return light ? .325f + .325f * multiplier : .42f + .28f * multiplier;
    case UiBackgroundTier::highest:
        return light ? .8f + .2f * multiplier : .39f + .26f * multiplier;
    default:
        return 1;
    }
}
inline bool ui_background_palette(const Theme& theme, Color* colors) noexcept {
    if (!colors || !ui_background_active(theme))
        return false;
    for (u32 i = 0; i < theme.background.color_count; ++i) {
        const u32 value = theme.background.colors[i];
        colors[i] = {((value >> 16) & 255) / 255.f, ((value >> 8) & 255) / 255.f, (value & 255) / 255.f, 1};
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
inline Color ui_background_surface_color(const Theme& theme, Color color, float opacity) noexcept {
    if (!ui_background_active(theme))
        return color;
    if (!(opacity >= 0))
        opacity = 0;
    opacity = minimum(opacity, 1.f);
    color.alpha *= opacity;
    return color;
}
// Hover adds one glass highlight; it must not add an intensity-dependent
// opaque canvas over the color underneath. The glass keeps the tone of the
// (neutral) color it is given but takes the hue of state.tint, shaded toward
// black or tinted toward white, so it reads as the theme rather than gray.
inline Color ui_background_hover_color(const Theme& theme, Color color, float opacity = .20f) noexcept {
    if (!ui_background_active(theme))
        return color;
    const Color hue = theme.tokens.state.tint;
    const auto tone = [](Color c) noexcept { return .2126f * c.red + .7152f * c.green + .0722f * c.blue; };
    const float want = tone(color), have = tone(hue);
    const float shade = want <= have ? (have > 0 ? want / have : 0) : 1;
    const float lift = want > have ? (want - have) / (1 - have) : 0;
    color.red = hue.red * shade + (1 - hue.red) * lift;
    color.green = hue.green * shade + (1 - hue.green) * lift;
    color.blue = hue.blue * shade + (1 - hue.blue) * lift;
    color.alpha *= opacity >= 0 ? minimum(opacity, 1.f) : 0;
    return color;
}
// Thin connector lines (reply arms, thread spines, a forward's edge): their
// usual tone in the theme's tint. Flat themes keep the colour.
inline Color ui_background_line_color(const Theme& theme) noexcept {
    return ui_background_hover_color(theme, theme.interactive_hover, 1);
}
inline Color ui_background_muted_text_color(const Theme& theme) noexcept {
    return theme.text_secondary;
}
} // namespace maicord
