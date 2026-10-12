#pragma once

#include "maicord/render/colr_program.hpp"

namespace maicord {
inline constexpr u32 kRegionalFlagSvgMaximumBytes = 64 * 1024;
// Licensed artwork from the same pinned Twemoji revision as the Unicode
// identity table, stored deflate-compressed in groups of adjacent flags. The
// group is inflated into caller-owned scratch of at least
// kRegionalFlagSvgMaximumBytes and the result borrows it, so concurrent
// callers need distinct scratch. Absent for unsupported sequences; no network work.
ConstSlice<u8> regional_flag_bundled_svg(u16 sequence, Slice<u8> scratch) noexcept;

inline constexpr u32 kRegionalFlagSvgMaximumLayers = 64;
inline constexpr u32 kRegionalFlagSvgMaximumCommands = 2048;
// Flags are compiled for (and rasterized into) 64 px cells of the 36-unit viewBox.
inline constexpr float kRegionalFlagPixelsPerUnit = 64.f / 36;

struct RegionalFlagSvgLayer {
    Utf8View path;
    ColrColor color;
    float cx, cy, rx, ry, rotation;
    GlyphFillRule fill_rule;
};
struct RegionalFlagSvg {
    RegionalFlagSvgLayer layers[kRegionalFlagSvgMaximumLayers];
    u32 count;
};

// Strict, non-executing subset verified against all 262 pinned Twemoji flags.
// The result borrows encoded bytes. No XML entities, external resources,
// opacity, gradients, strokes, masks or arbitrary transforms are admitted.
bool regional_flag_svg_parse(ConstSlice<u8> encoded, RegionalFlagSvg* output) noexcept;
bool regional_flag_svg_outline(const RegionalFlagSvgLayer& layer, GlyphOutline* output) noexcept;
bool regional_flag_svg_compile(const RegionalFlagSvg& image, ColrPaintProgram* program,
                               float pixels_per_unit) noexcept;

} // namespace maicord
