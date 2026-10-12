#pragma once
#include "maicord/text/sfnt.hpp"

namespace maicord {
// Prepared views borrow their enclosing mapped font bytes. Opens
// validate complete record spans and ordering. Trailing enclosing bytes are
// allowed. glyph_limit is an exclusive bound, 1..65536; use maxp.numGlyphs for
// real fonts. Callers must validate coverage counts/classes against lookup data.
// Record byte lengths are implied by validated format and record_count.
// Keep only the borrowed pointer; no duplicate extent or narrowing of counts.
struct SfntCoverage {
    const u8* records;
    u32 covered_count;
    u16 format,record_count;
};
struct SfntClassDef {
    const u8* records;
    u16 format,record_count,start_glyph,max_class;
};
struct SfntDevice {
    ConstSlice<u8> words;
    u16 start_or_outer,end_or_inner,format;
};
static_assert(sizeof(SfntCoverage)==16 && sizeof(SfntClassDef)==16 && sizeof(SfntDevice)==24);
inline constexpr u32 sfnt_not_covered=0xffffffffu;

SfntStatus sfnt_coverage_open(ConstSlice<u8> bytes,u32 glyph_limit,SfntCoverage* output) noexcept;
// Query only prepared immutable views. Missing glyphs return sfnt_not_covered.
u32 sfnt_coverage_index(const SfntCoverage& coverage,u16 glyph) noexcept;
SfntStatus sfnt_coverage_glyph(const SfntCoverage& coverage,u32 index,u16* glyph) noexcept;
SfntStatus sfnt_class_def_open(ConstSlice<u8> bytes,u32 glyph_limit,SfntClassDef* output) noexcept;
// Unassigned glyphs have class zero. A zero-initialized absent ClassDef is also
// all zero; this is useful for optional GDEF tables, not a substitute for open.
u16 sfnt_class_value(const SfntClassDef& classes,u16 glyph) noexcept;
SfntStatus sfnt_device_open(ConstSlice<u8> bytes,SfntDevice* output) noexcept;
// Device deltas are pixels. A VariationIndex is unsupported here:
// its caller must resolve font-unit deltas through the appropriate variation
// store before scaling. Outside a Device ppem interval the delta is zero.
SfntStatus sfnt_device_pixels(const SfntDevice& device,u16 ppem,i16* delta) noexcept;
bool sfnt_device_variation_index(const SfntDevice& device,u32* index) noexcept;
}
