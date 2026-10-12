#pragma once

#include "maicord/text/outline.hpp"

namespace maicord {

struct GlyphRasterBounds {
    i32 left;
    i32 top;
    i32 right;
    i32 bottom;
    u16 width;
    u16 height;
};

bool glyph_raster_bounds(const GlyphOutline& outline, float pixels_per_em, u16 padding,
                         GlyphRasterBounds* bounds) noexcept;
bool glyph_raster_bounds(float left, float top, float right, float bottom,
                         float pixels_per_em, u16 padding, GlyphRasterBounds* bounds) noexcept;

} // namespace maicord
