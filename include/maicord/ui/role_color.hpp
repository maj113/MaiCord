#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {
// Role colours are server data. The official client shows them exactly in every theme
// (no contrast or gradient-canvas remapping); only enhanced role styles add
// their gradient, which light themes dim with brightness(.85).
inline Color ui_role_color(u32 rgb) noexcept {
    return {static_cast<float>((rgb >> 16u) & 0xFFu) / 255.0f, static_cast<float>((rgb >> 8u) & 0xFFu) / 255.0f,
            static_cast<float>(rgb & 0xFFu) / 255.0f, 1.0f};
}

// Text effect kind for enhanced role styles: a linear-gradient (to
// right) of primary, secondary, primary, or primary, secondary, tertiary,
// primary for the holographic style, repeating every 100 DIPs from the name's
// left edge and clipped to the glyphs.
inline constexpr u8 kUiTextEffectRoleGradient = 9;
inline constexpr float kUiRoleGradientPeriod = 100.0f;

inline bool ui_role_gradient(const RoleRecord& role) noexcept {
    return role.color != 0 && role_color_present(role.secondary_color);
}

inline UiTextEffect ui_role_gradient_effect(const Theme& theme, const RoleRecord& role) noexcept {
    UiTextEffect effect{};
    if (!ui_role_gradient(role))
        return effect;
    const u32 colors[]{role.color, role.secondary_color, role.tertiary_color};
    const u8 count = role_color_present(role.tertiary_color) ? 3 : 2;
    const bool light = theme.appearance == ThemeAppearance::light;
    for (u8 i = 0; i < count; ++i) {
        u32 rgb = colors[i] & 0xFFFFFFu;
        if (light) {
            // Light themes dim the gradient name: filter: brightness(.85).
            const u32 r = ((rgb >> 16u) & 0xFFu) * 85u / 100u, g = ((rgb >> 8u) & 0xFFu) * 85u / 100u,
                      b = (rgb & 0xFFu) * 85u / 100u;
            rgb = (r << 16u) | (g << 8u) | b;
        }
        effect.colors[i] = rgb;
    }
    effect.kind = kUiTextEffectRoleGradient;
    effect.count = count;
    return effect;
}

// The role colour sample at x DIPs from the name's left edge; the
// glyph shader evaluates the same stops per pixel.
inline Color ui_role_gradient_sample(const UiTextEffect& effect, float x) noexcept {
    if (effect.kind != kUiTextEffectRoleGradient || effect.count < 2 || effect.count > 3)
        return ui_role_color(effect.colors[0]);
    float period = x / kUiRoleGradientPeriod;
    period -= static_cast<float>(static_cast<i32>(period));
    if (period < 0)
        period += 1;
    const float segment = period * effect.count;
    const u32 a = minimum(static_cast<u32>(segment), static_cast<u32>(effect.count) - 1u);
    const u32 b = a + 1u < effect.count ? a + 1u : 0u;
    const float t = segment - static_cast<float>(a);
    const Color from = ui_role_color(effect.colors[a]), to = ui_role_color(effect.colors[b]);
    return {from.red + (to.red - from.red) * t, from.green + (to.green - from.green) * t,
            from.blue + (to.blue - from.blue) * t, 1.0f};
}
} // namespace maicord
