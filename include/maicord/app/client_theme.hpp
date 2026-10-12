#pragma once
#include "maicord/app/client_preferences.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/ui/background_color_math.hpp"
#include "maicord/ui/theme_styles.hpp"

namespace maicord {
inline Theme client_theme_projection(const ClientPreferences& preferences) noexcept {
    Theme theme =
        preferences.theme == ClientTheme::light || preferences.theme == ClientTheme::custom_light_gradient
            ? kLightTheme
        : preferences.theme == ClientTheme::amoled ? kAmoledTheme
                                                   : kDefaultTheme;
    if (preferences.theme == ClientTheme::light || preferences.theme < ClientTheme::aurora ||
        (preferences.theme > ClientTheme::custom_gradient &&
         preferences.theme != ClientTheme::custom_light_gradient)) {
        // Effect choice is independent of appearance. The effect-only
        // descriptor leaves all flat palette tokens and gradient materials
        // unchanged; designated canvases use their own actual fill color.
        if (background_theme_valid(preferences.gradient) &&
            preferences.gradient.effect != BackgroundEffect::none) {
            theme.background = {};
            theme.background.effect = preferences.gradient.effect;
            theme.background.speed = preferences.gradient.speed;
            theme.background.strength = preferences.gradient.strength;
            theme.background.density = preferences.gradient.density;
        }
        return theme;
    }
    theme.background =
        background_theme_valid(preferences.gradient) ? preferences.gradient : background_theme_defaults();
    const u32* palette = nullptr;
    constexpr u32 aurora[3]{0x6935b8, 0x164d80, 0x16836f};
    constexpr u32 sunset[3]{0x7a284b, 0xa65732, 0x362062};
    constexpr u32 ocean[3]{0x15335f, 0x167d89, 0x363269};
    if (preferences.theme == ClientTheme::aurora)
        palette = aurora;
    else if (preferences.theme == ClientTheme::sunset)
        palette = sunset;
    else if (preferences.theme == ClientTheme::ocean)
        palette = ocean;
    if (palette) {
        theme.background.color_count = 3;
        for (u32 i = 0; i < 5; ++i)
            theme.background.colors[i] = i < 3 ? palette[i] : 0;
    }
    BackgroundColorAnchors anchors{};
    if (background_color_anchors(theme.background, preferences.theme == ClientTheme::custom_light_gradient,
                                 &anchors)) {
        const auto base = background_color_oklab(anchors.base), text = background_color_oklab(anchors.text);
        Color* surfaces[]{&theme.base_lowest,       &theme.base_lower,        &theme.base_low,
                          &theme.surface,           &theme.surface_high,      &theme.interactive,
                          &theme.interactive_hover, &theme.avatar_placeholder};
        for (auto* surface : surfaces)
            *surface = background_color_mix(*surface, base, anchors.base_percent);
        theme.text_primary = background_color_mix(theme.text_primary, text, anchors.text_percent);
        theme.text_secondary = background_color_mix(theme.text_secondary, text, anchors.text_percent);
    }
    theme = ui_theme_resolve_defaults(theme);
    // Accent treatment follows the gradient; flat themes return above unchanged.
    ui_theme_resolve_emphasis(&theme);
    return theme;
}
} // namespace maicord
