#pragma once
#include "maicord/ui/paint.hpp"
namespace maicord {
enum class UiIcon : u8 {
#define MAICORD_ICON(name, source) name,
#include "maicord/ui/icon_ids.inc"
#undef MAICORD_ICON
    count,
};
static_assert(static_cast<u32>(UiIcon::count) < 255);

// Paint-quad encoding resolved by the renderer's icon atlas. The quad carries
// the nominal DIP box; the renderer snaps it to whole pixels and writes the
// atlas origin into reserved_x/y and the pixel scale into radius.
inline constexpr float kPaintIconMode = -9.0f;
struct UiIconQuadData {
    u32 icon;
    bool solid;
    float weight;
};
constexpr bool ui_icon_quad(const PaintQuad& quad) noexcept {
    return quad.border_width == kPaintIconMode;
}
constexpr UiIconQuadData ui_icon_quad_data(const PaintQuad& quad) noexcept {
    return {static_cast<u32>(quad.border.red), quad.border.green != 0, quad.border.blue};
}

enum class UiIconRole : u8 {
    normal,
    muted,
    accent,
    danger,
    success,
    warning,
    on_accent,
    count
};
struct UiIconStyle {
    Color ink;
    Color fill;   // Alpha zero draws the outline only; otherwise a solid underlay.
    float weight; // Stroke multiplier; zero selects the theme icon weight.
};
// Lucide-derived and original icons compiled into the MIPK pack.
bool ui_icon_paint(PaintStream* paint, UiIcon icon, UiRect bounds, Color color) noexcept;
bool ui_icon_paint_styled(PaintStream* paint, UiIcon icon, UiRect bounds, const UiIconStyle& style) noexcept;
Color ui_icon_role_color(const Theme& theme, UiIconRole role) noexcept;
float ui_icon_theme_weight(const Theme& theme) noexcept;
// Largest standard extent (16, 20 or 24 DIP) that fits a control slot.
float ui_icon_button_extent(float slot) noexcept;
// Optical stroke in pixels, quantized to quarter pixels: 1.5 DIP up to 20 DIP,
// 2 DIP at 24 DIP, growing slowly for larger artwork.
float ui_icon_stroke_pixels(float extent_dip, float pixel_scale, float weight) noexcept;
} // namespace maicord
