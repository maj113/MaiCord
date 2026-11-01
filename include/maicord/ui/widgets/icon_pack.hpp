#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct GlyphOutline;

// MIPK v1, written by tools/generate_icon_pack.py. Coordinates are 1/64 units
// on a 24-unit grid.
inline constexpr u32 kIconPackGrid = 24;
inline constexpr u32 kIconPackUnits = 64;
inline constexpr u32 kIconMaximumOutlineCommands = 512;
inline constexpr u32 kIconMaximumStems = 8;

struct IconPack {
    const u8* data;
    u32 size;
    u32 icon_count;
};

// Grid-unit stems recorded by the compiler for translation-only pixel fitting.
struct IconStem {
    float position;
    u8 axis; // 0: vertical stem at x, 1: horizontal stem at y.
    u8 length;
};

// Pixel-space placement: x = unit * scale + offset_x.
struct IconOutlineTransform {
    float scale;
    float offset_x;
    float offset_y;
    bool fill_closed; // Closed stroked contours are also filled (solid variant).
};

// The built-in pack, inflated on first use; empty if inflation failed.
const IconPack& icon_pack_builtin() noexcept;
u32 icon_pack_stems(const IconPack& pack, u32 index, IconStem* stems, u32 capacity) noexcept;
// Writes begin/line/cubic/end commands into outline->commands (capacity bound).
bool icon_pack_decode(const IconPack& pack, u32 index, const IconOutlineTransform& transform,
                      GlyphOutline* outline) noexcept;

} // namespace maicord
