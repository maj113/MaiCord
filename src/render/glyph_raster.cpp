#include "maicord/render/glyph_raster.hpp"

namespace maicord {
namespace {

bool bounded_float(float value, float limit) noexcept {
    return value == value && value >= -limit && value <= limit;
}

i32 floor_i32(float value) noexcept {
    const i32 truncated = static_cast<i32>(value);
    return static_cast<float>(truncated) > value ? truncated - 1 : truncated;
}

i32 ceil_i32(float value) noexcept {
    const i32 truncated = static_cast<i32>(value);
    return static_cast<float>(truncated) < value ? truncated + 1 : truncated;
}

} // namespace

bool glyph_raster_bounds(const GlyphOutline& outline, float pixels_per_em, u16 padding,
                         GlyphRasterBounds* bounds) noexcept {
    return outline.has_bounds && outline.count && outline.commands &&
           glyph_raster_bounds(outline.minimum_x, outline.minimum_y, outline.maximum_x,
                               outline.maximum_y, pixels_per_em, padding, bounds);
}
bool glyph_raster_bounds(float minimum_x, float minimum_y, float maximum_x, float maximum_y,
                         float pixels_per_em, u16 padding, GlyphRasterBounds* bounds) noexcept {
    if (!bounds ||
        !bounded_float(pixels_per_em, 1024.0f) || pixels_per_em <= 0.0f || padding > 64 ||
        !bounded_float(minimum_x, 64.0f) || !bounded_float(minimum_y, 64.0f) ||
        !bounded_float(maximum_x, 64.0f) || !bounded_float(maximum_y, 64.0f) ||
        maximum_x < minimum_x || maximum_y < minimum_y) {
        return false;
    }
    const i32 left = floor_i32(minimum_x * pixels_per_em) - padding;
    const i32 top = floor_i32(minimum_y * pixels_per_em) - padding;
    const i32 right = ceil_i32(maximum_x * pixels_per_em) + padding;
    const i32 bottom = ceil_i32(maximum_y * pixels_per_em) + padding;
    const i32 width = right - left;
    const i32 height = bottom - top;
    if (width <= 0 || height <= 0 || width > 65'535 || height > 65'535) {
        return false;
    }
    *bounds = GlyphRasterBounds{left, top, right, bottom, static_cast<u16>(width),
                                static_cast<u16>(height)};
    return true;
}

} // namespace maicord
