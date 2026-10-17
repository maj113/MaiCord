#pragma once
#include "maicord/text/gsub.hpp"

namespace maicord {
// Reads and matches require successfully prepared, unchanged views and mappings.
// Borrowed, validated record; device offsets use the immediate parent, including
// PairSet for PairPos format 1. Scalar values stay packed in the font mapping.
struct SfntValueRecord {ConstSlice<u8> parent;u32 offset;u16 format,bytes;};
struct SfntPositionValue {i16 design[4];SfntDevice devices[4];};
struct SfntAnchor {ConstSlice<u8> table;};
struct SfntAnchorValue {SfntDevice devices[2];i16 x,y;u16 format,point;};
struct SfntPositionInstance {const VariationInstance* variations;u16 ppem_x,ppem_y;bool variable;VariationArithmetic arithmetic;bool round_variable_anchors=false;
    // Explicit script policy: reserved bit 1 marks a type-4 attachment whose
    // resolved component owner survives reordering/presentation substitution.
    bool source_mark_targets=false;};
struct SfntPositionAdjustment {double design[4];i16 pixels[4];};
static_assert(sizeof(SfntValueRecord)==24 && sizeof(SfntPositionValue)==104 && sizeof(SfntAnchor)==16 && sizeof(SfntAnchorValue)==56);
static_assert(sizeof(SfntPositionInstance)==16 && sizeof(SfntPositionAdjustment)==40);
// Returns 0xffff for reserved bits, otherwise 0..16 bytes (zero is meaningful).
u16 sfnt_position_value_size(u16 format) noexcept;
SfntStatus sfnt_value_record_read(const SfntValueRecord& record,SfntPositionValue* output) noexcept;
SfntStatus sfnt_anchor_open(ConstSlice<u8> table,SfntAnchor* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus sfnt_anchor_read(const SfntAnchor& anchor,SfntAnchorValue* output) noexcept;
// Keeps font-unit and pixel corrections separate: no scaling/rounding or writing
// direction is guessed. VariationIndex is active only for variable fonts;
// classic Device deltas only for non-variable fonts. A required missing variation
// instance is an error, except for the no-variation sentinel. Values are x/y
// placement, then x/y advance. Output is valid only on success.
SfntStatus sfnt_position_value_resolve(const SfntPositionValue& value,const SfntPositionInstance& instance,SfntPositionAdjustment* output) noexcept;
// Anchor format 2 exposes both design coordinates and the contour point index;
// choosing/resolving the hinted or varied outline point is the outline owner's job.

struct SfntGposSubtable {
    ConstSlice<u8> table;SfntCoverage coverage;SfntClassDef classes[2];u32 glyph_limit;
    u16 counts[2];u8 type,format,value_formats[2],value_bytes[2];
    u16 last_covered_glyph=65535; // Prepared bound, stored in tail padding.
};
struct SfntGposMatch {SfntValueRecord values[2];u32 first_input,second_input,next_input;bool matched;};
struct SfntGposLookup {ConstSlice<SfntGposSubtable> subtables;SfntLookupFilter filter;};
static_assert(sizeof(SfntGposSubtable)==80 && sizeof(SfntGposMatch)==64 && sizeof(SfntGposLookup)==32);
// Single/pair positioning types 1/2, formats 1/2. Opens validate all coverage,
// classes, pair ordering and referenced devices. Scalar-only matrices need one
// span check, not a walk over every value; an empty record has zero encoded size.
// Prepared views and mappings remain immutable; work reports share a caller budget.
SfntStatus sfnt_gpos_subtable_open(ConstSlice<u8> table,u16 type,u32 glyph_limit,SfntGposSubtable* output,
                                   u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Logical glyph order, bounded input range; first input is always tested, and
// intervening filtered glyphs survive. An empty second value format leaves the
// second glyph available as the next input; a present zero value still consumes
// that input. Class indices outside encoded matrices produce no match.
// No positions mutate.
SfntStatus sfnt_gpos_subtable_match_view(const SfntGposSubtable& subtable,const SfntGdef& gdef,const SfntLookupFilter& filter,
                                        SfntGlyphView glyphs,u32 start,u32 input_end,SfntGposMatch* output,
                                        u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
}
