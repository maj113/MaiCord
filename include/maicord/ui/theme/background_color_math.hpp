#pragma once
#include "maicord/ui/theme/theme.hpp"

namespace maicord {
struct BackgroundHsl {
    float hue, saturation, lightness;
};
struct BackgroundOklab {
    double lightness, a, b;
};
inline float background_color_unit(float value) noexcept {
    return value >= 0 ? minimum(value, 1.f) : 0;
}
// Fixed, local arithmetic avoids a CRT math import or lookup-table owner. All
// callers use the closed unit interval and degree 2, 3 or 5: a square root, or
// Newton's method with a fixed step count.
double background_color_root(double value, u32 degree) noexcept;
double background_color_linear(float channel) noexcept;
float background_color_srgb(double channel) noexcept;
double background_color_luminance(const Color& color) noexcept;
BackgroundHsl background_color_hsl(const Color& color) noexcept;
Color background_color_from_hsl(BackgroundHsl hsl) noexcept;
BackgroundOklab background_color_oklab(const Color& color) noexcept;
Color background_color_from_oklab(BackgroundOklab lab, float alpha = 1) noexcept;
// CSS color-mix normalizes original 100% plus tint B%; B is not the final share.
Color background_color_mix(const Color& original, BackgroundOklab anchor, float tint_percent) noexcept;
struct BackgroundColorAnchors {
    Color base, text;
    float base_percent, text_percent;
};
bool background_color_anchors(const BackgroundTheme& draft, bool light,
                              BackgroundColorAnchors* output) noexcept;
u32 background_color_pack(const Color& color) noexcept;
u32 background_color_rotate_hue(u32 rgb, float degrees) noexcept;
u32 background_color_lrgb_midpoint(u32 first, u32 second) noexcept;
bool background_theme_add_color(const BackgroundTheme& draft, BackgroundTheme* output) noexcept;
u16 background_theme_snap_angle(float angle) noexcept;
u32 background_color_random_index(u32& state, u32 count) noexcept;
// Randomized harmony palette: only palette/angle/intensity change; the caller's
// effect profile stays intact.
bool background_theme_randomized(const BackgroundTheme& draft, u32 seed,
                                 BackgroundTheme* output) noexcept;
} // namespace maicord
