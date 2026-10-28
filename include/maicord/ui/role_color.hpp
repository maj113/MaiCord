#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {
// Role colours are server data and show exactly in every theme (no contrast or
// gradient-canvas remapping); only enhanced role styles add their gradient,
// which light themes dim to 85% brightness.
inline Color ui_role_color(u32 rgb) noexcept {
    return color_from_rgb24(rgb);
}

// Text effect kind for enhanced role styles: a left-to-right linear gradient of
// primary, secondary, primary (primary, secondary, tertiary, primary for the
// holographic style), repeating every 100 DIPs from the name's left edge and
// clipped to the glyphs.
inline constexpr u8 kUiTextEffectRoleGradient = 9;

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
            // Light themes dim the gradient name to 85% brightness.
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
} // namespace maicord
