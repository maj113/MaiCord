#pragma once
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/theme_styles.hpp"

namespace maicord {
// One elevation ladder for every surface that floats above a view canvas.
// Gradient themes paint these with the global-gradient surface material: each
// step adds a fixed lift of visible gradient above the canvas (capped by the
// text contrast check) and gets a palette edge. Flat themes keep the opaque fill
// and border of the same role. Callers name a role and never pick tiers, shares,
// materials or border tones, so new overlays inherit the styling.
enum class UiElevation : u8 {
    card,    // inline timeline cards: embeds, audio, polls, attachments
    banner,  // in-view bars and pills: unread / jump to present, notices
    popover, // menus, context menus, autocomplete, pickers, choosers, header popouts
    dialog,  // modal dialogs and confirmations, switcher, forward, downloads, editors
    tooltip, // tooltips and toasts
    count
};
inline constexpr u32 kUiElevationCount = static_cast<u32>(UiElevation::count);

struct UiSurfaceStyle {
    Color fill, border;
    float border_width;
    // Visible gradient share of the surface material (0 on flat themes).
    float reveal;
    UiBackgroundSurfaceMode mode;
    UiBackgroundBorderTone border_tone;
};

// Gradient share added above the view canvas (the chat tier) per elevation.
inline constexpr float kUiElevationLift[]{.04f, .12f, .16f, .19f, .22f};
static_assert(sizeof(kUiElevationLift) / sizeof(kUiElevationLift[0]) == kUiElevationCount);

// Largest share <= reveal at which normal text still keeps 4.5:1 over every
// gradient stop composed onto the appearance plate (the theme contrast check).
inline float ui_elevation_readable_reveal(const Theme& theme, float reveal) noexcept {
    Color stops[5]{};
    if (!ui_background_palette(theme, stops))
        return 0;
    const float plate = theme.appearance == ThemeAppearance::light ? 1.f : 0.f;
    const auto readable = [&](float share) noexcept {
        for (u32 i = 0; i < theme.background.color_count; ++i) {
            const Color composed{plate + (stops[i].red - plate) * share, plate + (stops[i].green - plate) * share,
                                 plate + (stops[i].blue - plate) * share, 1};
            if (ui_theme_contrast_ratio(theme.tokens.text.normal, composed) < 4.5f)
                return false;
        }
        return true;
    };
    if (readable(reveal))
        return reveal;
    float low = 0, high = reveal;
    for (u32 i = 0; i < 8; ++i) {
        const float middle = (low + high) * .5f;
        (readable(middle) ? low : high) = middle;
    }
    return low;
}

// Share of the gradient visible on the surface (0 on flat themes): the
// canvas share plus the elevation's lift, capped for text contrast. Monotonic
// non-decreasing in elevation for both appearances and every intensity.
inline float ui_elevation_gradient_share(const Theme& theme, UiElevation elevation) noexcept {
    if (!ui_background_active(theme) || elevation >= UiElevation::count)
        return 0;
    const float canvas = 1 - ui_background_tier_opacity(theme, UiBackgroundTier::chat);
    return ui_elevation_readable_reveal(theme,
                                        minimum(.8f, canvas + kUiElevationLift[static_cast<u32>(elevation)]));
}

inline UiSurfaceStyle ui_surface_style(const Theme& theme, UiElevation elevation) noexcept {
    const bool gradient = ui_background_active(theme);
    UiSurfaceStyle style{theme.base_lower,
                         theme.interactive,
                         1,
                         ui_elevation_gradient_share(theme, elevation),
                         gradient ? UiBackgroundSurfaceMode::gradient : UiBackgroundSurfaceMode::tinted,
                         gradient ? UiBackgroundBorderTone::palette : UiBackgroundBorderTone::semantic};
    switch (elevation) {
    case UiElevation::banner:
        style.fill = theme.surface_high;
        style.border = theme.tokens.border.subtle;
        break;
    case UiElevation::dialog:
        style.border = theme.tokens.border.strong;
        break;
    case UiElevation::tooltip:
        style.fill = theme.base_lowest;
        style.border = theme.base_low;
        break;
    default:
        break;
    }
    return style;
}

// Paint any plate with an elevation's material: gradient themes sample the
// global gradient at the elevation's share; flat themes paint fill/border.
__declspec(noinline) inline bool ui_surface_paint_material(PaintStream* stream, UiRect rectangle,
                                                           const Theme& theme, UiElevation elevation,
                                                           float radius, Color fill, Color border,
                                                           float border_width) noexcept {
    if (!stream || elevation >= UiElevation::count)
        return false;
    const auto style = ui_surface_style(theme, elevation);
    const u32 first = stream->count;
    if (!ui_background_paint_surface(stream, rectangle, theme, fill, border, radius, border_width, 1, style.mode,
                                     UiBackgroundTier::highest, style.border_tone))
        return false;
    if (style.mode == UiBackgroundSurfaceMode::gradient && stream->count != first)
        stream->quads[first].reserved_y = style.reveal;
    return true;
}

// Drop shadow under a floating surface: six plates widening 2 DIP each, offset
// downward so the falloff reads as a blur rather than a ring. Light themes use
// the soft shadow token, dark and gradient-dark themes the denser one (a black
// falloff over a dark canvas needs more weight to read). Only popovers and
// dialogs cast one; cards, banners and tooltips stay flat.
inline constexpr u32 kUiSurfaceShadowLayers = 6;
inline constexpr float kUiSurfaceShadowStep = 2, kUiSurfaceShadowOffset = 4;
// Dark themes drop the shadow further (12:8 ratio).
inline float ui_surface_shadow_offset(const Theme& theme) noexcept {
    return kUiSurfaceShadowOffset * (theme.appearance == ThemeAppearance::light ? 1.f : 1.5f);
}
inline bool ui_surface_casts_shadow(UiElevation elevation) noexcept {
    return elevation == UiElevation::popover || elevation == UiElevation::dialog;
}
inline Color ui_surface_shadow_color(const Theme& theme, UiElevation elevation, float alpha = 1) noexcept {
    const bool light = theme.appearance == ThemeAppearance::light;
    // Per-plate share; the plates stack to ~1x (popover) and ~1.4x (dialog)
    // of the token right at the surface edge.
    const float share = alpha * (elevation == UiElevation::dialog ? .24f : .17f);
    return ui_theme_color_alpha(light ? theme.tokens.overlay.shadow_soft : theme.tokens.overlay.shadow, share);
}
__declspec(noinline) inline bool ui_surface_shadow_paint(PaintStream* stream, UiRect rectangle,
                                                         const Theme& theme, UiElevation elevation,
                                                         float radius, float alpha = 1) noexcept {
    if (!ui_surface_casts_shadow(elevation) || !(alpha > 0))
        return true;
    const Color shadow = ui_surface_shadow_color(theme, elevation, alpha);
    const float offset = ui_surface_shadow_offset(theme);
    for (u32 i = kUiSurfaceShadowLayers; i > 0; --i) {
        const float grow = kUiSurfaceShadowStep * i;
        if (!paint_stream_push_rounded(stream, rectangle.left - grow, rectangle.top - grow + offset,
                                       rectangle.right + grow, rectangle.bottom + grow + offset, shadow,
                                       radius + grow))
            return false;
    }
    return true;
}

// Extent of the shadow plates around a surface rectangle.
inline UiRect ui_surface_shadow_bounds(UiRect rectangle, const Theme& theme) noexcept {
    const float grow = kUiSurfaceShadowStep * kUiSurfaceShadowLayers, offset = ui_surface_shadow_offset(theme);
    return {rectangle.left - grow, rectangle.top - grow + offset, rectangle.right + grow,
            rectangle.bottom + grow + offset};
}
// For surfaces whose content is clipped to the panel (menus, popouts,
// editors): the shadow paints first in its own clip, which also keeps it in a
// batch of its own above the base layer; then ui_surface_paint_plate.
inline bool ui_surface_shadow_paint_isolated(PaintStream* stream, UiRect rectangle, const Theme& theme,
                                             UiElevation elevation, float radius, float alpha = 1) noexcept {
    if (!ui_surface_casts_shadow(elevation) || !(alpha > 0))
        return true;
    const auto bounds = ui_surface_shadow_bounds(rectangle, theme);
    return stream && paint_stream_push_clip(stream, bounds) &&
           ui_surface_shadow_paint(stream, rectangle, theme, elevation, radius, alpha) &&
           paint_stream_pop_clip(stream);
}
// The elevation's plate without its shadow (see ui_surface_paint).
inline bool ui_surface_paint_plate(PaintStream* stream, UiRect rectangle, const Theme& theme, UiElevation elevation,
                                   float radius, const Color* plain_fill = nullptr,
                                   const Color* plain_border = nullptr) noexcept {
    const auto style = ui_surface_style(theme, elevation);
    return ui_surface_paint_material(stream, rectangle, theme, elevation, radius,
                                     plain_fill ? *plain_fill : style.fill,
                                     plain_border ? *plain_border : style.border, style.border_width);
}

// Paint a floating surface for its elevation. plain_fill/plain_border replace
// only the flat-theme colours (and alpha, for fades) of a caller with an
// established look; gradient themes always use the elevation material.
// Popovers and dialogs cast ui_surface_shadow_paint on every theme.
inline bool ui_surface_paint(PaintStream* stream, UiRect rectangle, const Theme& theme, UiElevation elevation,
                             float radius, const Color* plain_fill = nullptr,
                             const Color* plain_border = nullptr) noexcept {
    return ui_surface_shadow_paint(stream, rectangle, theme, elevation, radius, plain_fill ? plain_fill->alpha : 1.f) &&
           ui_surface_paint_plate(stream, rectangle, theme, elevation, radius, plain_fill, plain_border);
}

// Selected rows/tabs/pills: the emphasis tint gains alpha with elevation on a
// gradient theme, the way higher surfaces gain gradient contrast. The
// resolver reserved contrast headroom for the full boost.
inline Color ui_emphasis_selected(const Theme& theme, UiElevation elevation = UiElevation::card) noexcept {
    Color selected = theme.tokens.emphasis.selected;
    if (!ui_background_active(theme))
        return selected;
    const float top = ui_elevation_gradient_share(theme, UiElevation::tooltip);
    const float share = ui_elevation_gradient_share(theme, elevation);
    const float boost = top > 0 ? 1 + (kUiThemeEmphasisElevationBoost - 1) * (share / top) : 1;
    selected.alpha = minimum(1.f, selected.alpha * boost);
    return selected;
}
} // namespace maicord
