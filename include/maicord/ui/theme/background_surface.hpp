#pragma once
#include "maicord/ui/theme/background_presentation.hpp"
#include "maicord/ui/core/paint.hpp"

namespace maicord {
enum class UiBackgroundSurfaceMode : u8 {
    tinted,
    gradient
};
enum class UiBackgroundBorderTone : u8 {
    semantic,
    palette
};
// Every canvas quad carries this in reserved_x (unread by its shader paths) so
// the renderer finds canvases for the effect pass on any theme.
inline constexpr float kUiBackgroundCanvasMarker = -8;
inline bool ui_background_canvas_quad(const PaintQuad& quad) noexcept {
    return quad.reserved_x == kUiBackgroundCanvasMarker && (quad.border_width == -8 || quad.border_width == 0);
}
// Designated view canvases draw their own effects. The base is composed first
// from the flat fill or global gradient/tier; the renderer's effect pass then
// draws the selected effect over it, before the widgets that follow. Static
// widgets and modal surfaces use the surface material below.
__declspec(noinline) inline bool ui_background_paint_canvas(
    PaintStream* stream, const UiRect& rectangle, const Theme& theme, const Color& fill,
    UiBackgroundTier tier = UiBackgroundTier::chat) noexcept {
    if (!stream || static_cast<u8>(tier) > static_cast<u8>(UiBackgroundTier::highest))
        return false;
    const u32 first = stream->count;
    if (!paint_stream_push(stream, rectangle, fill))
        return false;
    if (stream->count == first)
        return true;
    auto& quad = stream->quads[first];
    quad.reserved_x = kUiBackgroundCanvasMarker;
    if (ui_background_active(theme) || background_effects_active(theme.background)) {
        quad.border_width = -8;
        quad.reserved_y = ui_background_active(theme) ? 1 - ui_background_tier_opacity(theme, tier) : 0;
    }
    return true;
}
// Static caller tokens remain opaque; only gradientable controls
// sample the global palette. Semantic borders keep focus/status/brand RGB.
__declspec(noinline) inline bool ui_background_paint_surface(
    PaintStream* stream, const UiRect& rectangle, const Theme& theme, const Color& fill, const Color& border, float radius,
    float border_width, float palette_share = .9f,
    UiBackgroundSurfaceMode mode = UiBackgroundSurfaceMode::tinted,
    UiBackgroundTier tier = UiBackgroundTier::highest,
    UiBackgroundBorderTone border_tone = UiBackgroundBorderTone::semantic) noexcept {
    if (!stream || !(palette_share >= 0 && palette_share <= 1) ||
        static_cast<u8>(mode) > static_cast<u8>(UiBackgroundSurfaceMode::gradient) ||
        static_cast<u8>(tier) > static_cast<u8>(UiBackgroundTier::highest) ||
        static_cast<u8>(border_tone) > static_cast<u8>(UiBackgroundBorderTone::palette))
        return false;
    const u32 first = stream->count;
    if (!paint_stream_push_rounded_border(stream, rectangle, fill, border, radius, border_width))
        return false;
    if (stream->count != first && ui_background_active(theme) &&
        mode == UiBackgroundSurfaceMode::gradient) {
        auto& quad = stream->quads[first];
        quad.reserved_x =
            border_tone == UiBackgroundBorderTone::palette ? -quad.border_width : quad.border_width;
        quad.reserved_y = (1 - ui_background_tier_opacity(theme, tier)) * palette_share;
        quad.border_width = -7;
    }
    return true;
}
// An inset rounded card on a lower background tier (a category rail, an
// inspector bar) that floats in its panel with a gap on every side, as the
// message composer sits in the message pane. On background themes the card
// samples the gradient at the tier's opacity like the panel itself; flat
// themes paint fill.
inline bool ui_background_paint_card(PaintStream* stream, UiRect card, const Theme& theme, const Color& fill,
                                     float radius, UiBackgroundTier tier) noexcept {
    return !ui_rect_has_area(card) ||
           ui_background_paint_surface(stream, card, theme, fill, fill, radius, 0, .9f,
                                       UiBackgroundSurfaceMode::gradient, tier);
}
} // namespace maicord
