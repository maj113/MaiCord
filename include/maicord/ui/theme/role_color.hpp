#pragma once
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/theme/theme.hpp"

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
// clipped to the glyphs. While the message or member row is hovered (`animate`)
// the official client moves it 100 DIPs to the right every 1.5 s and adds a 4-DIP
// glow at 70% (all.css gradientUsernameAnimation, convenienceGlowGradientActive);
// the renderer does both only in full motion (glyph_draw.hlsl).
inline constexpr u8 kUiTextEffectRoleGradient = 9;

// The official client paints the gradient only for a role with primary and secondary colours in a server that has
// ENHANCED_ROLE_COLORS; a role keeps its colours after the server loses the feature and then shows the primary one.
inline bool ui_role_gradient(const ChatStateStore& state, const RoleRecord& role) noexcept {
    if (role.color == 0 || !role_color_present(role.secondary_color))
        return false;
    const auto* guild = chat_state_store_find_guild(state, role.guild_id);
    return guild && guild->enhanced_role_colors;
}

inline UiTextEffect ui_role_gradient_effect(const Theme& theme, const ChatStateStore& state, const RoleRecord& role,
                                            bool animate = false, u64 started_ms = 0) noexcept {
    UiTextEffect effect{};
    if (!ui_role_gradient(state, role))
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
    effect.animate = animate;
    effect.started_ms = started_ms;
    return effect;
}
} // namespace maicord
