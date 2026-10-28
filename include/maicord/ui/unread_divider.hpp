#pragma once
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/unread_divider_style.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {
inline constexpr Utf8View kUiUnreadDividerLabel = utf8_literal("New messages");
inline bool ui_unread_divider_style_valid(const UiUnreadDividerStyle& style) noexcept {
    return ui_extent_valid(style.height) && style.height >= 16 && style.height <= 96 &&
        ui_extent_valid(style.horizontal_inset) && style.horizontal_inset <= 96 &&
        ui_extent_valid(style.label_width) && style.label_width >= 32 && style.label_width <= 256 &&
        ui_extent_valid(style.label_gap) && style.label_gap <= 32 &&
        ui_extent_valid(style.line_thickness) && style.line_thickness >= 1 && style.line_thickness <= 4;
}
struct UiUnreadDividerGeometry { UiRect line, label; };
// Nearest device-pixel edge for a DIP coordinate at pixel_scale (dpi / 96).
inline float ui_unread_divider_snap(float dip, float pixel_scale) noexcept {
    if (!(pixel_scale > 0))
        return dip;
    const float pixels = dip * pixel_scale;
    return static_cast<float>(static_cast<i64>(pixels + (pixels < 0 ? -.5f : .5f))) / pixel_scale;
}
// A positive pixel_scale snaps the hairline to whole device pixels at least
// one pixel thick, so it stays crisp at fractional DPI and while scrolling.
inline UiUnreadDividerGeometry ui_unread_divider_geometry(UiRect rectangle,
    const UiUnreadDividerStyle& style = kDefaultUiUnreadDividerStyle, float pixel_scale = 0) noexcept {
    if (!ui_rect_has_area(rectangle) || !ui_unread_divider_style_valid(style)) return {};
    const float inset = minimum(style.horizontal_inset, ui_rect_width(rectangle) / 4);
    const float left = rectangle.left + inset, right = rectangle.right - inset;
    const float label_left = maximum(left, right - style.label_width);
    const float center = (rectangle.top + rectangle.bottom) * .5f;
    UiUnreadDividerGeometry geometry{{left, center - style.line_thickness * .5f,
             maximum(left, label_left - style.label_gap), center + style.line_thickness * .5f},
            {label_left, rectangle.top, right, rectangle.bottom}};
    if (pixel_scale > 0 && ui_extent_valid(pixel_scale)) {
        auto& line = geometry.line;
        const float pixels = maximum(1.f, static_cast<float>(static_cast<i64>(style.line_thickness * pixel_scale + .5f)));
        const float extent = pixels / maximum(pixel_scale, 1e-3f);
        line.top = ui_unread_divider_snap(center - extent * .5f, pixel_scale);
        line.bottom = line.top + extent;
        line.left = ui_unread_divider_snap(line.left, pixel_scale);
        line.right = maximum(line.left, ui_unread_divider_snap(line.right, pixel_scale));
    }
    return geometry;
}
inline Color ui_unread_divider_text_color(const Theme& theme) noexcept {
    return ui_foreground_color(theme, theme.notification);
}
inline bool ui_unread_divider_paint(PaintStream* paint, UiRect rectangle, const Theme& theme,
    const UiUnreadDividerStyle& style = kDefaultUiUnreadDividerStyle, float pixel_scale = 0) noexcept {
    const auto geometry = ui_unread_divider_geometry(rectangle, style, pixel_scale);
    return !ui_rect_has_area(geometry.line) || paint_stream_push_rounded_border(paint, geometry.line, theme.notification, theme.notification, 0, 0);
}
} // namespace maicord
