#pragma once

#include "maicord/render/glyph_quad.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord {

enum class GlyphQuadBoundsError : u8 {
    none,
    invalid_input,
    invalid_clip,
    invalid_quad,
};

struct GlyphQuadBoundsResult {
    u32 kept;
    u32 dropped;
    u32 clipped;
    GlyphQuadBoundsError error;

    constexpr bool ok() const noexcept {
        return error == GlyphQuadBoundsError::none;
    }
};

// Transactionally validates and clips one appended glyph range to `clip`.
// Invalid geometry leaves the stream untouched. Fully clipped quads are
// removed and partial quads get matching UV adjustment.
GlyphQuadBoundsResult glyph_quad_clip_range(
    GlyphQuad* quads, u32 first, u32* count, UiRect clip) noexcept;

// Center the complete ink bounds, not the font's line box. All color layers
// move together; their UVs, scale and relative positions remain unchanged.
bool glyph_quad_center_range(GlyphQuad* quads, u32 first, u32 count, UiRect owner) noexcept;

} // namespace maicord
