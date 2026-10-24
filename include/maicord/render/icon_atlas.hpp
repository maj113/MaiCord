#pragma once

#include "maicord/render/glyph_atlas_gpu.hpp"
#include "maicord/ui/icon_pack.hpp"
#include "maicord/ui/paint.hpp"

namespace maicord {

inline constexpr u16 kIconAtlasExtent = 512;
inline constexpr u32 kIconAtlasEntries = 512;
inline constexpr u32 kIconAtlasRows = 64;
inline constexpr u32 kIconAtlasMaximumPixels = 255;

// Dedicated R8 atlas for pack icons, separate from text so icon churn never
// evicts glyphs. Cells are keyed by icon, variant, raster size and stroke.
struct IconAtlas {
    GlyphAtlasGpu gpu;
    GlyphAtlasAllocator allocator;
    GlyphAtlasEntry entries[kIconAtlasEntries];
    GlyphAtlasRow rows[kIconAtlasRows];
    GlyphPathCommand commands[kIconMaximumOutlineCommands];
    u32 resolved_dpi;
    float resolved_weight;
    u32 rasterized;
    u32 unresolved;
    bool ready;
};

struct IconPlacement {
    i32 left, top;
    u16 size;
    float stroke_half_width;
    float offset_x, offset_y;
};

bool icon_atlas_initialize(IconAtlas* atlas, ID3D11Device* device) noexcept;
void icon_atlas_release(IconAtlas* atlas) noexcept;
bool icon_atlas_stale(const IconAtlas& atlas, u32 dpi, float weight) noexcept;
// Copies all quads into the mapped instance buffer, resolving icons with
// snapped rectangles and atlas origins and rasterizing missing cells first.
// With valid buffers, failure still copies the unresolved tail unchanged.
bool icon_atlas_resolve(IconAtlas* atlas, ID3D11DeviceContext* context, const PaintQuad* source,
                        PaintQuad* destination, u32 count, u32 dpi, float weight) noexcept;

} // namespace maicord
