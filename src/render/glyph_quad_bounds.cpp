#include "maicord/render/glyph_quad_bounds.hpp"

namespace maicord {
namespace {

bool finite_value(float value) noexcept {
    return value == value && value >= -kMaximumUiCoordinate &&
           value <= kMaximumUiCoordinate;
}

bool unit_value(float value) noexcept {
    return value == value && value >= 0.0f && value <= 1.0f;
}

bool valid_clip(UiRect rectangle) noexcept {
    return finite_value(rectangle.left) && finite_value(rectangle.top) &&
           finite_value(rectangle.right) && finite_value(rectangle.bottom) &&
           ui_rect_has_area(rectangle);
}

bool valid_quad(const GlyphQuad& quad) noexcept {
    const float color_offset = quad.u0 >= 2.f ? 2.f : 0.f;
    return finite_value(quad.left) && finite_value(quad.top) &&
           finite_value(quad.right) && finite_value(quad.bottom) &&
           quad.right > quad.left && quad.bottom > quad.top &&
           unit_value(quad.u0 - color_offset) && unit_value(quad.v0) &&
           unit_value(quad.u1 - color_offset) && unit_value(quad.v1) &&
           quad.u1 >= quad.u0 && quad.v1 >= quad.v0 &&
           finite_value(quad.color.red) &&
           finite_value(quad.color.green) &&
           finite_value(quad.color.blue) &&
           finite_value(quad.color.alpha);
}

GlyphQuad clipped_quad(const GlyphQuad& source, UiRect clip) noexcept {
    GlyphQuad result = source;
    const float inverse_width = 1.0f / (source.right - source.left);
    const float inverse_height = 1.0f / (source.bottom - source.top);
    const float left = maximum(source.left, clip.left);
    const float top = maximum(source.top, clip.top);
    const float right = minimum(source.right, clip.right);
    const float bottom = minimum(source.bottom, clip.bottom);
    const float u_extent = source.u1 - source.u0;
    const float v_extent = source.v1 - source.v0;
    result.u0 = source.u0 + (left - source.left) * inverse_width * u_extent;
    result.v0 = source.v0 + (top - source.top) * inverse_height * v_extent;
    result.u1 = source.u0 + (right - source.left) * inverse_width * u_extent;
    result.v1 = source.v0 + (bottom - source.top) * inverse_height * v_extent;
    result.left = left;
    result.top = top;
    result.right = right;
    result.bottom = bottom;
    return result;
}

} // namespace

bool glyph_quad_center_range(GlyphQuad* quads, u32 first, u32 count, UiRect owner) noexcept {
    if (!quads || first > count || !valid_clip(owner)) return false;
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

GlyphQuadBoundsResult glyph_quad_clip_range(
    GlyphQuad* quads, u32 first, u32* count, UiRect clip) noexcept {
    if (!quads || !count || first > *count) {
        return GlyphQuadBoundsResult{0, 0, 0,
                                     GlyphQuadBoundsError::invalid_input};
    }
    if (!valid_clip(clip)) {
        return GlyphQuadBoundsResult{0, 0, 0,
                                     GlyphQuadBoundsError::invalid_clip};
    }
    const u32 original_count = *count;
    for (u32 index = first; index < original_count; ++index) {
        if (!valid_quad(quads[index])) {
            return GlyphQuadBoundsResult{0, 0, 0,
                                         GlyphQuadBoundsError::invalid_quad};
        }
    }

    u32 write = first;
    u32 dropped = 0;
    u32 clipped = 0;
    for (u32 index = first; index < original_count; ++index) {
        const GlyphQuad& source = quads[index];
        const UiRect intersection{
            maximum(source.left, clip.left), maximum(source.top, clip.top),
            minimum(source.right, clip.right),
            minimum(source.bottom, clip.bottom)};
        if (!ui_rect_has_area(intersection)) {
            ++dropped;
            continue;
        }
        const bool changed = intersection.left != source.left ||
                             intersection.top != source.top ||
                             intersection.right != source.right ||
                             intersection.bottom != source.bottom;
        quads[write++] = changed ? clipped_quad(source, clip) : source;
        clipped += changed ? 1u : 0u;
    }
    *count = write;
    return GlyphQuadBoundsResult{write - first, dropped, clipped,
                                 GlyphQuadBoundsError::none};
}

} // namespace maicord
