#pragma once
#include "maicord/text/glyph_buffer.hpp"
namespace maicord {
struct SfntNominalRange {u32 scalar_count,utf16_units,examined;bool simple;};
// Classify the first range in decoded text from its already mapped nominal
// glyphs and the selected font's base-feature participation bitmap (empty means
// no participating glyphs; otherwise one bit per font glyph). The compatibility
// profile distinguishes initial look-ahead from interior combining-mark
// backtracking; this is not grapheme segmentation.
// Lengths are returned only on success; examined is diagnostic on failure.
// Invalid examined scalars, missing capacity and exhausted scalar work are
// distinct errors. No allocation; input is not modified.
SfntStatus sfnt_nominal_range(ConstSlice<u32> scalars,SfntGlyphView nominal_glyphs,
    ConstSlice<u64> participation,u32 glyph_limit,SfntNominalRange* output,u32 scalar_work=1000000) noexcept;
}
