#pragma once

#include "maicord/core/base.hpp"
#include "maicord/ui/background_theme.hpp"
#include "maicord/ui/transition.hpp"

namespace maicord {

struct Color {
    float red;
    float green;
    float blue;
    float alpha;
};

// Colour of a 0xRRGGBB value; of a 0xRRGGBBAA value.
constexpr Color color_from_rgb24(u32 rgb, float alpha = 1.0f) noexcept {
    return {static_cast<float>((rgb >> 16) & 255) / 255.0f, static_cast<float>((rgb >> 8) & 255) / 255.0f,
            static_cast<float>(rgb & 255) / 255.0f, alpha};
}
constexpr Color color_from_rgba(u32 rgba) noexcept {
    return {static_cast<float>(rgba >> 24) / 255.0f, static_cast<float>((rgba >> 16) & 255) / 255.0f,
            static_cast<float>((rgba >> 8) & 255) / 255.0f, static_cast<float>(rgba & 255) / 255.0f};
}

struct UiResolvedThemeStyles;
enum class ThemeAppearance : u32 {
    dark,
    light
};

// Semantic tier (maicord/ui/theme_tokens.inc). Every token is resolved once
// when a theme is built or imported; consumers read plain fields.
#define MAICORD_THEME_GROUP(group) struct UiTheme_##group##_tokens {
#define MAICORD_THEME_TOKEN(group, name, dark, light) Color name;
#define MAICORD_THEME_GROUP_END(group)                                                                     \
    }                                                                                                      \
    ;
#include "maicord/ui/theme_tokens.inc"
struct UiThemeTokens {
#define MAICORD_THEME_GROUP(group) UiTheme_##group##_tokens group;
#include "maicord/ui/theme_tokens.inc"
};
struct UiThemeScale {
#define MAICORD_THEME_SCALE(name, value, minimum, maximum) float name{value};
#include "maicord/ui/theme_tokens.inc"
};
struct UiThemeMotion {
#define MAICORD_THEME_MOTION(name, duration, curve_name) UiMotionStyle name{duration, UiMotionCurve::curve_name};
#include "maicord/ui/theme_tokens.inc"
};

struct Theme {
    // Core palette tier. Existing consumers and v1 documents address these.
    Color base_lowest;
    Color base_lower;
    Color base_low;
    Color surface;
    Color surface_high;
    Color interactive;
    Color interactive_hover;
    Color accent;
    Color text_primary;
    Color avatar_placeholder;
    Color notification;
    Color syntax_keyword;
    Color syntax_type;
    Color syntax_string;
    Color syntax_number;
    Color syntax_comment;
    Color syntax_operator;
    Color syntax_builtin;
    Color syntax_preprocessor;
    Color text_secondary;
    UiThemeTokens tokens{};
    UiThemeScale scale{};
    UiThemeMotion motion{};
    BackgroundTheme background{};
    ThemeAppearance appearance{ThemeAppearance::dark};
    const UiResolvedThemeStyles* styles{};
    u64 style_revision{};
};

constexpr Color ui_theme_color_alpha(Color color, float alpha) noexcept {
    color.alpha *= alpha;
    return color;
}

// The built-in presets (theme_presets.hpp), resolved once at compile time in
// theme.cpp rather than in every source that includes this header.
extern const Theme kDefaultTheme;
extern const Theme kLightTheme;
// kDefaultTheme on near-black surfaces, built on demand: storing it would
// double the resolved-theme footprint for four changed colours.
Theme ui_theme_amoled() noexcept;

} // namespace maicord
