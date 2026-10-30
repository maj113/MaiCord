#include "maicord/render/glyph_quad_bounds.hpp"

namespace maicord {
namespace {

bool unit_value(float value) noexcept {
    return value == value && value >= 0.0f && value <= 1.0f;
}

bool valid_clip(UiRect rectangle) noexcept {
    return ui_coordinate_valid(rectangle.left) && ui_coordinate_valid(rectangle.top) &&
           ui_coordinate_valid(rectangle.right) && ui_coordinate_valid(rectangle.bottom) &&
           ui_rect_has_area(rectangle);
}

bool valid_quad(const GlyphQuad& quad) noexcept {
    const float color_offset = quad.u0 >= 2.f ? 2.f : 0.f;
    return ui_coordinate_valid(quad.left) && ui_coordinate_valid(quad.top) &&
           ui_coordinate_valid(quad.right) && ui_coordinate_valid(quad.bottom) &&
           quad.right > quad.left && quad.bottom > quad.top &&
           unit_value(quad.u0 - color_offset) && unit_value(quad.v0) &&
           unit_value(quad.u1 - color_offset) && unit_value(quad.v1) &&
           quad.u1 >= quad.u0 && quad.v1 >= quad.v0 &&
           ui_coordinate_valid(quad.color.red) &&
           ui_coordinate_valid(quad.color.green) &&
           ui_coordinate_valid(quad.color.blue) &&
           ui_coordinate_valid(quad.color.alpha);
}

} // namespace

bool glyph_quad_center_range(GlyphQuad* quads, u32 first, u32 count, UiRect owner) noexcept {
    if (!valid_clip(owner)) return false;
    if (first == count) return true;
    UiRect ink{quads[first].left, quads[first].top, quads[first].right, quads[first].bottom};
    for (u32 index = first; index < count; ++index) {
        if (!valid_quad(quads[index])) return false;
        ink.left = minimum(ink.left, quads[index].left);
        ink.top = minimum(ink.top, quads[index].top);
        ink.right = maximum(ink.right, quads[index].right);
        ink.bottom = maximum(ink.bottom, quads[index].bottom);
    }
    const float dx = ((owner.left + owner.right) - (ink.left + ink.right)) * 0.5f;
    const float dy = ((owner.top + owner.bottom) - (ink.top + ink.bottom)) * 0.5f;
    for (u32 index = first; index < count; ++index) {
        quads[index].left += dx;
        quads[index].right += dx;
        quads[index].top += dy;
        quads[index].bottom += dy;
    }
    return true;
}

bool glyph_quad_clip_range(GlyphQuad* quads, u32 first, u32* count, UiRect clip) noexcept {
    if (!valid_clip(clip)) {
        return false;
    }
    const u32 original_count = *count;
    for (u32 index = first; index < original_count; ++index) {
        if (!valid_quad(quads[index])) {
            return false;
        }
    }

    u32 write = first;
    for (u32 index = first; index < original_count; ++index) {
        const GlyphQuad& source = quads[index];
        const UiRect intersection{
            maximum(source.left, clip.left), maximum(source.top, clip.top),
            minimum(source.right, clip.right),
            minimum(source.bottom, clip.bottom)};
        if (!ui_rect_has_area(intersection))
            continue;
        const bool changed = intersection.left != source.left ||
                             intersection.top != source.top ||
                             intersection.right != source.right ||
                             intersection.bottom != source.bottom;
        if (changed) {
            const float inverse_width = 1.0f / (source.right - source.left);
            const float inverse_height = 1.0f / (source.bottom - source.top);
            const float u_extent = source.u1 - source.u0;
            const float v_extent = source.v1 - source.v0;
            quads[write] = {intersection.left, intersection.top, intersection.right, intersection.bottom,
                source.u0 + (intersection.left - source.left) * inverse_width * u_extent,
                source.v0 + (intersection.top - source.top) * inverse_height * v_extent,
                source.u0 + (intersection.right - source.left) * inverse_width * u_extent,
                source.v0 + (intersection.bottom - source.top) * inverse_height * v_extent, source.color};
        } else if (write != index) {
            quads[write] = source;
        }
        ++write;
    }
    *count = write;
    return true;
}

} // namespace maicord
