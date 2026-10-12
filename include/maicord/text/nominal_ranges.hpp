#pragma once
#include "maicord/text/glyph_buffer.hpp"
namespace maicord {
struct SfntNominalRange {u32 scalar_count,utf16_units,examined;bool simple;};
// Classify the first range in decoded text using its already mapped nominal
// glyphs and the selected font's base-feature participation bitmap. Mapping is
// reused from font/fallback resolution, not repeated during this scan. Empty
// participation means no participating glyphs. Otherwise one bit per font glyph.
// The compatibility profile distinguishes initial look-ahead from
// interior combining-mark backtracking; this is not grapheme segmentation.
// Return lengths only on success. examined is diagnostic on failure. Invalid
// examined scalars, missing capacity and exhausted scalar-work are distinct errors.
// No allocation, mutable input, glyph edits, font lookup or OS service.
SfntStatus sfnt_nominal_range(ConstSlice<u32> scalars,SfntGlyphView nominal_glyphs,
    ConstSlice<u64> participation,u32 glyph_limit,SfntNominalRange* output,u32 scalar_work=1000000) noexcept;
}
