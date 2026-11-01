#pragma once
// The color glyph owner behind D3D11GlyphTextOwner::color; the GPU footprint probes read its resources.
#include "maicord/render/effects/colr_paint.hpp"
#include "maicord/render/d3d11/glyph_text.hpp"

namespace maicord {
struct D3D11ColorGlyphOwner {
    ColrPaintProgram program;
    ColrPaintGpu gpu;
    // Direct-mapped dependency hints. Identity includes face and glyph; a
    // collision only causes a rebuild, never reuse with the wrong foreground.
    u32 foreground_hints[256];
};
} // namespace maicord
