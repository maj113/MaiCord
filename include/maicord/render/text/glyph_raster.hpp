#pragma once

#include "maicord/text/opentype/outline.hpp"

namespace maicord {

struct GlyphRasterBounds {
    i32 left;
    i32 top;
    i32 right;
    i32 bottom;
    u16 width;
    u16 height;
};

// Pixel bounds of the outline at pixels_per_em with one pixel of padding on each side.
bool glyph_raster_bounds(const GlyphOutline& outline, float pixels_per_em, GlyphRasterBounds* bounds) noexcept;
bool glyph_raster_bounds(float left, float top, float right, float bottom, float pixels_per_em,
                         GlyphRasterBounds* bounds) noexcept;

} // namespace maicord
