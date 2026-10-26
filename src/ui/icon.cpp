#include "maicord/ui/icon.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/theme_styles.hpp"
namespace maicord {
namespace {
bool push_icon_quad(PaintStream* paint, UiIcon icon, const UiRect& box, const Color& color, bool solid,
                    float weight) noexcept {
    const u32 before = paint->count;
    if (!paint_stream_push(paint, box, color))
        return false;
    if (paint->count != before) {
        auto& quad = paint->quads[before];
        quad.border = {static_cast<float>(static_cast<u32>(icon)), solid ? 1.f : 0.f, weight, 0};
        quad.radius = 0;
        quad.border_width = kPaintIconMode;
        quad.reserved_x = -1;
        quad.reserved_y = -1;
    }
    return true;
}
Color unpack(u32 rgba) noexcept {
    return {((rgba >> 24) & 255) / 255.f, ((rgba >> 16) & 255) / 255.f, ((rgba >> 8) & 255) / 255.f,
            (rgba & 255) / 255.f};
}
} // namespace

bool ui_icon_paint_styled(PaintStream* paint, UiIcon icon, const UiRect& bounds, const UiIconStyle& style) noexcept {
    if (!paint || icon >= UiIcon::count || !ui_rect_has_area(bounds) || !(style.weight >= 0 && style.weight <= 4))
        return false;
    const float extent = minimum(ui_rect_width(bounds), ui_rect_height(bounds));
    const float x = (bounds.left + bounds.right - extent) * .5f, y = (bounds.top + bounds.bottom - extent) * .5f;
    const UiRect box{x, y, x + extent, y + extent};
    return (style.fill.alpha <= 0 || push_icon_quad(paint, icon, box, style.fill, true, style.weight)) &&
           push_icon_quad(paint, icon, box, style.ink, false, style.weight);
}

bool ui_icon_paint(PaintStream* paint, UiIcon icon, const UiRect& bounds, const Color& color) noexcept {
    return ui_icon_paint_styled(paint, icon, bounds, {color, {}, 0});
}

Color ui_icon_role_color(const Theme& theme, UiIconRole role) noexcept {
    const u32 index = static_cast<u32>(role);
    if (index >= static_cast<u32>(UiIconRole::count))
        return theme.text_primary;
    if (theme.styles && theme.styles->icons.colors[index])
        return unpack(theme.styles->icons.colors[index]);
    switch (role) {
    case UiIconRole::muted:
        return theme.text_secondary;
    case UiIconRole::accent:
        return theme.tokens.emphasis.outline;
    case UiIconRole::danger:
        return ui_foreground_color(theme, theme.notification);
    case UiIconRole::success:
        return ui_foreground_color(theme, theme.tokens.accent.success);
    case UiIconRole::warning:
        return ui_foreground_color(theme, theme.tokens.accent.warning);
    case UiIconRole::on_accent:
        return ui_surface_foreground(theme, theme.tokens.emphasis.fill);
    default:
        return theme.text_primary;
    }
}

float ui_icon_theme_weight(const Theme& theme) noexcept {
    const float weight = theme.styles ? theme.styles->icons.weight : 1.f;
    return weight >= .5f && weight <= 2.f ? weight : 1.f;
}

float ui_icon_button_extent(float slot) noexcept {
    return slot >= 32 ? 24.f : slot >= 26 ? 20.f : 16.f;
}

float ui_icon_stroke_pixels(float extent_dip, float pixel_scale, float weight) noexcept {
    const float dip = extent_dip <= 20 ? 1.5f
                      : extent_dip <= 24 ? 1.5f + (extent_dip - 20) * .125f
                                         : minimum(3.f, 2.f + (extent_dip - 24) * .05f);
    const float pixels = dip * pixel_scale * (weight > 0 ? weight : 1.f);
    return maximum(1.f, static_cast<float>(static_cast<i32>(pixels * 4 + .5f)) * .25f);
}
} // namespace maicord
