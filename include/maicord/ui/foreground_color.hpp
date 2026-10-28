#pragma once
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/background_color_math.hpp"

namespace maicord {
namespace foreground_color_detail {
// The candidate with the most contrast on a fill of this luminance, when it
// keeps normal-text 4.5:1.
inline bool readable(const Color* candidates, u32 count, double background, Color* output) noexcept {
    double best = 0;
    for (u32 i = 0; i < count; ++i) {
        const double value = background_color_luminance(candidates[i]);
        const double contrast = (maximum(value, background) + .05) / (minimum(value, background) + .05);
        if (contrast > best) {
            *output = candidates[i];
            best = contrast;
        }
    }
    return best >= 4.5;
}
template <u32 Count>
inline bool readable(const Color (&candidates)[Count], double background, Color* output) noexcept {
    return readable(candidates, Count, background, output);
}
} // namespace foreground_color_detail
// Choose an authored palette color for a filled control, in order: the
// theme's text colors, its on-accent text (white on the stock accents), then
// a background color for fills that need a dark label (bright custom
// accents), each only when it keeps normal-text contrast; otherwise an opaque
// endpoint. Text wins over backgrounds, so AMOLED's pure black base never
// labels its blue accent. The bounded sRGB math adds no theme state or CRT.
inline Color ui_surface_foreground(const Theme& theme, Color fill) noexcept {
    const double background = background_color_luminance(fill);
    const Color text[]{theme.text_primary, theme.text_secondary};
    const Color accent[]{theme.tokens.emphasis.text};
    const Color backgrounds[]{theme.base_lowest, theme.surface};
    Color selected{};
    if (foreground_color_detail::readable(text, background, &selected) ||
        foreground_color_detail::readable(accent, background, &selected) ||
        foreground_color_detail::readable(backgrounds, background, &selected))
        return selected;
    const double white = 1.05 / (background + .05), black = (background + .05) / .05;
    return white >= black ? Color{1, 1, 1, 1} : Color{0, 0, 0, 1};
}
// Preserve semantic hue while keeping authored role colors readable on the
// admitted gradient canvas. This runs when text is published, not on hover.
inline Color ui_foreground_color(const Theme& theme, Color color) noexcept {
    const bool light = theme.appearance == ThemeAppearance::light;
    if (!light && !ui_background_active(theme))
        return color;
    const float luminance = ui_background_luminance(color);
    if ((light && luminance <= .10f) || (!light && luminance >= .50f))
        return color;
    float lower = 0, upper = 1;
    const Color target = light ? Color{0, 0, 0, color.alpha} : Color{1, 1, 1, color.alpha};
    for (u32 i = 0; i < 10; ++i) {
        const float blend = (lower + upper) * .5f;
        const Color trial{color.red + (target.red - color.red) * blend,
                          color.green + (target.green - color.green) * blend,
                          color.blue + (target.blue - color.blue) * blend, color.alpha};
        const float value = ui_background_luminance(trial);
        if (light ? value > .10f : value < .50f)
            lower = blend;
        else
            upper = blend;
    }
    return {color.red + (target.red - color.red) * upper,
            color.green + (target.green - color.green) * upper,
            color.blue + (target.blue - color.blue) * upper, color.alpha};
}
} // namespace maicord
