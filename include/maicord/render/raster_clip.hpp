#pragma once

#include "maicord/ui/geometry.hpp"

namespace maicord {

struct RasterClipRect {
    i32 left;
    i32 top;
    i32 right;
    i32 bottom;
};

// All paint, text, media and damage clips must use the same conservative
// DIP-to-pixel conversion. Truncating right/bottom loses the last touched
// pixel at fractional DPI, and makes text disagree with its background.
inline RasterClipRect raster_clip_rectangle(const UiRect& rectangle, float pixel_scale, u32 width,
                                            u32 height) noexcept {
    if (!ui_coordinate_valid(rectangle.left) || !ui_coordinate_valid(rectangle.top) ||
        !ui_coordinate_valid(rectangle.right) || !ui_coordinate_valid(rectangle.bottom) ||
        !ui_rect_has_area(rectangle) || !(pixel_scale > 0.0f && pixel_scale <= 1'024.0f) || width == 0 ||
        height == 0 || width > 16'777'216u || height > 16'777'216u) {
        return RasterClipRect{};
    }
    const float pixel_width = static_cast<float>(width);
    const float pixel_height = static_cast<float>(height);
    // Clamp before converting to integers, including entirely offscreen clips.
    const float left = maximum(0.0f, minimum(rectangle.left * pixel_scale, pixel_width));
    const float top = maximum(0.0f, minimum(rectangle.top * pixel_scale, pixel_height));
    const float right = maximum(0.0f, minimum(rectangle.right * pixel_scale, pixel_width));
    const float bottom = maximum(0.0f, minimum(rectangle.bottom * pixel_scale, pixel_height));
    if (!(left < right && top < bottom))
        return RasterClipRect{};
    const i32 right_floor = static_cast<i32>(right);
    const i32 bottom_floor = static_cast<i32>(bottom);
    return RasterClipRect{static_cast<i32>(left), static_cast<i32>(top),
                          right_floor + (static_cast<float>(right_floor) < right ? 1 : 0),
                          bottom_floor + (static_cast<float>(bottom_floor) < bottom ? 1 : 0)};
}

} // namespace maicord
