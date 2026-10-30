#pragma once

#include "maicord/render/glyph_quad.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord {

// Transactionally validates and clips one appended glyph range to `clip`;
// false (invalid clip or quad) leaves the stream untouched. Fully clipped
// quads are removed and partial quads get matching UV adjustment.
bool glyph_quad_clip_range(GlyphQuad* quads, u32 first, u32* count, UiRect clip) noexcept;

// Center the complete ink bounds, not the font's line box. All color layers
// move together; their UVs, scale and relative positions remain unchanged.
bool glyph_quad_center_range(GlyphQuad* quads, u32 first, u32 count, UiRect owner) noexcept;

} // namespace maicord
