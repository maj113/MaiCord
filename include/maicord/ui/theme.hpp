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
// Rebuild every semantic token from its table default over the current core
// palette. Call after mutating core colours; document overrides use
// ui_theme_tokens_resolve (theme_styles.hpp) instead.
constexpr Theme ui_theme_resolve_defaults(Theme theme) noexcept {
    const bool light = theme.appearance == ThemeAppearance::light;
#define P(core) theme.core
#define PA(core, a) ui_theme_color_alpha(theme.core, a)
#define T(group, name) theme.tokens.group.name
#define RGBA(r, g, b, a) Color{r, g, b, a}
#define MAICORD_THEME_TOKEN(group, name, dark, light_value) theme.tokens.group.name = light ? light_value : dark;
#include "maicord/ui/theme_tokens.inc"
#undef P
#undef PA
#undef T
#undef RGBA
    return theme;
}

// The core table is data, not renderer policy. The theme file parser can
// replace any tier without changing UI or rendering code.
inline constexpr Theme kDefaultTheme = ui_theme_resolve_defaults(Theme{
    {0.0588f, 0.0588f, 0.0706f, 1.0f}, {0.1020f, 0.1020f, 0.1176f, 1.0f}, {0.1333f, 0.1333f, 0.1451f, 1.0f},
    {0.1647f, 0.1647f, 0.1804f, 1.0f}, {0.2078f, 0.2078f, 0.2275f, 1.0f}, {0.2667f, 0.2667f, 0.2941f, 1.0f},
    {0.3216f, 0.3216f, 0.3529f, 1.0f}, {0.3451f, 0.3961f, 0.9490f, 1.0f}, {0.9490f, 0.9569f, 0.9686f, 1.0f},
    {0.3804f, 0.3882f, 0.4196f, 1.0f}, {0.9490f, 0.3216f, 0.3216f, 1.0f}, {0.7765f, 0.4706f, 0.8667f, 1.0f},
    {0.8980f, 0.7529f, 0.4706f, 1.0f}, {0.5961f, 0.7647f, 0.4745f, 1.0f}, {0.8196f, 0.6039f, 0.4000f, 1.0f},
    {0.4235f, 0.4667f, 0.5333f, 1.0f}, {0.6706f, 0.6980f, 0.7490f, 1.0f}, {0.3804f, 0.6863f, 0.9373f, 1.0f},
    {0.3373f, 0.7137f, 0.7608f, 1.0f}, {0.7098f, 0.7294f, 0.7569f, 1.0f},
});

inline constexpr Theme kAmoledTheme = []() constexpr {
    Theme theme = kDefaultTheme;
    theme.base_lowest = {0, 0, 0, 1};
    theme.base_lower = {.02f, .02f, .025f, 1};
    theme.base_low = {.035f, .035f, .045f, 1};
    theme.surface = {.06f, .06f, .075f, 1};
    theme.surface_high = {.105f, .105f, .125f, 1};
    return ui_theme_resolve_defaults(theme);
}();

inline constexpr Theme kLightTheme = []() constexpr {
    Theme theme = kDefaultTheme;
    theme.appearance = ThemeAppearance::light;
    theme.base_lowest = {.91f, .92f, .95f, 1};
    theme.base_lower = {.95f, .96f, .98f, 1};
    theme.base_low = {.975f, .98f, .99f, 1};
    theme.surface = {1, 1, 1, 1};
    theme.surface_high = {.93f, .94f, .97f, 1};
    theme.interactive = {.85f, .87f, .91f, 1};
    theme.interactive_hover = {.79f, .81f, .87f, 1};
    theme.text_primary = {.11f, .12f, .15f, 1};
    theme.text_secondary = {.36f, .38f, .43f, 1};
    theme.avatar_placeholder = {.67f, .69f, .74f, 1};
    theme.syntax_keyword = {.47f, .16f, .56f, 1};
    theme.syntax_type = {.48f, .34f, .08f, 1};
    theme.syntax_string = {.18f, .43f, .16f, 1};
    theme.syntax_number = {.57f, .28f, .12f, 1};
    theme.syntax_comment = {.43f, .46f, .51f, 1};
    theme.syntax_operator = {.34f, .37f, .43f, 1};
    theme.syntax_builtin = {.18f, .38f, .65f, 1};
    theme.syntax_preprocessor = {.16f, .44f, .49f, 1};
    return ui_theme_resolve_defaults(theme);
}();

} // namespace maicord
