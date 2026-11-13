#pragma once
#include "maicord/text/opentype/gpos.hpp"
#include "maicord/text/shaping/clusters.hpp"
#include "maicord/text/opentype/lookup_bounds.hpp"
namespace maicord {
// Font-unit state keeps scaling out of lookup execution. Fractional values allow
// resolved variable metrics/deltas.
struct SfntPosition {double advance,advance_offset,ascender_offset;};
struct SfntPlacement {float advance,advance_offset,ascender_offset;};
enum SfntPositionFlags : u8 {sfnt_position_zero_advance=1,sfnt_position_cluster_start=2};
struct SfntPositionExecution {u64 operations,matches;};
// A contextual child applies only at start and bypasses initial filtering; its
// other inputs still obey the lookup filter and input_end.
enum class SfntPositionApplication : u8 {scan,selected};
static_assert(sizeof(SfntPosition)==24 && sizeof(SfntPlacement)==12 && sizeof(SfntPositionExecution)==16);
// Nominal position: a zero-advance glyph keeps no advance and no offset in
// either direction (HarfBuzz zeroes mark and default-ignorable advances alone).
inline SfntPosition sfnt_position_nominal(double advance,bool zero) noexcept {
    return {zero?0:advance,0,0};
}
// Horizontal natural placement: ignore classic Device pixels, resolve variable
// font-unit corrections, and skip initial inputs according to lookup flags.
// Positions must have been initialized for these glyphs/direction and remain
// finite. Each matched pair is applied as a whole; earlier matches can remain after
// a failure. After a failure the caller must discard the attempt, not retry in place.
SfntStatus sfnt_position_execute_lookup(const SfntGposLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,
                                         Slice<SfntPosition> positions,u32 start,u32 input_end,bool rtl,
                                         const SfntPositionInstance& instance,SfntPositionExecution* result,
                                         u64 work_limit=1000000,u32 match_limit=1000000,
                                         SfntPositionApplication application=SfntPositionApplication::scan,
                                         const SfntLookupBounds* bounds=nullptr) noexcept;
// Legacy 'kern' table (version 0): true when every subtable header is in bounds and one
// is a horizontal, not cross-stream format-0 pair list, the ones HarfBuzz applies when
// GPOS has no kern feature (hb-ot-kern-table.hh); then sets the bit of every glyph
// (below bitmap.count*64) that one of those pairs names. The last pair list may
// outgrow its 16-bit length; the table end bounds it.
bool sfnt_legacy_kern_open(ConstSlice<u8> table,Slice<u64> bitmap) noexcept;
// Applies a valid table to positioned glyphs in logical order as HarfBuzz's kern machine:
// each glyph pairs with the next one without the zero-advance flag (marks and hidden
// controls); the first of a pair takes half the value (rounded down) as advance, the
// second the rest as advance and placement, both as GPOS value records would.
SfntStatus sfnt_legacy_kern_apply(ConstSlice<u8> table,SfntGlyphView glyphs,ConstSlice<u8> flags,Slice<SfntPosition> positions,bool rtl,u64* work) noexcept;
// Whether trailing spacing is finite and not negative (-0 is accepted).
bool sfnt_trailing_spacing_valid(float spacing) noexcept;
// Nonnegative trailing spacing in reading order, with zero leading/minimum
// spacing. RTL adjusts the first eligible glyph/offset; LTR adjusts the last.
// Placements and flags are validated shaping output; empty flags mean one
// independent nominal glyph per cluster. Flags do not overlap placements. Mutates
// only advances/advance offsets. Do not use the result of a failed attempt.
SfntStatus sfnt_trailing_spacing(Slice<SfntPlacement> placements,
    ConstSlice<u8> flags,float trailing_spacing,u64 work_limit=1000000,bool rtl=false) noexcept;
// Read-only finalization zeroes the advance of each zero-advance glyph (unless
// retain_zero_positioning) and leaves every offset as positioned, as HarfBuzz does;
// it converts to binary32 font units and multiplies by binary32 em/upem once; no
// pixel rounding. Output is valid only on success; positions are never changed.
// Empty runs are allowed.
SfntStatus sfnt_position_finalize(ConstSlice<SfntPosition> positions,ConstSlice<u8> flags,float em_size,u16 units_per_em,
                                    Slice<SfntPlacement> output,u64 work_limit=1000000,bool retain_zero_positioning=false) noexcept;
}
