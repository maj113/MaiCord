#pragma once
#include "maicord/text/gpos.hpp"
#include "maicord/text/clusters.hpp"
namespace maicord {
// Font-unit state keeps scaling out of lookup execution. Fractional values allow
// resolved variable metrics/deltas; Windows variable arithmetic parity is separate.
struct SfntPosition {double advance,advance_offset,ascender_offset;};
struct SfntPlacement {float advance,advance_offset,ascender_offset;};
using SfntAnchorPointResolver=SfntStatus(*)(void* context,u16 glyph,u16 point,double* x,double* y) noexcept;
enum SfntPositionFlags : u8 {sfnt_position_zero_advance=1,sfnt_position_cluster_start=2};
struct SfntPositionExecution {u64 operations,matches,adjusted_glyphs;};
// A contextual child applies only at start and bypasses initial filtering; its
// other inputs still obey the lookup filter and input_end.
enum class SfntPositionApplication : u8 {scan,selected};
static_assert(sizeof(SfntPosition)==24 && sizeof(SfntPlacement)==12 && sizeof(SfntPositionExecution)==24);
// Project current source-cluster transitions and caller-resolved final classes
// into natural-horizontal position flags. Interleaved membership can start the
// same source cluster more than once in glyph order. Records begin with
// SfntClusterGlyph; extra metadata is ignored. Default zero advance follows mark
// classification. An optional per-glyph 0/1 array supplies an explicit script
// policy instead (e.g. controls/spacing marks). No Unicode policy is inferred.
// Arrays are disjoint, except zero_advance may be output's prefix for
// in-place conversion from 0/1 policy bytes to flags. Partial overlap is invalid.
// Outputs are valid only on success. No allocation.
SfntStatus sfnt_position_flags_from_clusters(const SfntClusterMap& clusters,SfntGlyphView glyphs,Slice<u8> output,
                                             ConstSlice<u8> zero_advance={},u64 work_limit=1000000) noexcept;
// Flags are supplied by the shaping layer, not inferred from Unicode or GDEF.
// The first glyph must begin a cluster. Storage must not overlap other inputs.
SfntStatus sfnt_position_initialize(ConstSlice<double> nominal_advances,ConstSlice<u8> flags,bool rtl,
                                      Slice<SfntPosition> output,u64 work_limit=1000000) noexcept;
// Horizontal natural placement: ignore classic Device pixels, resolve variable
// font-unit corrections, and skip initial inputs according to lookup flags.
// Positions must have been initialized for these glyphs/direction and remain
// finite. Each matched pair is applied as a whole; earlier matches can remain after
// a failure. After a failure the caller must discard the attempt, not retry in place.
SfntStatus sfnt_position_execute_lookup(const SfntGposLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,
                                         Slice<SfntPosition> positions,u32 start,u32 input_end,bool rtl,
                                         const SfntPositionInstance& instance,SfntPositionExecution* result,
                                         u64 work_limit=1000000,u32 match_limit=1000000,
                                         SfntPositionApplication application=SfntPositionApplication::scan) noexcept;
// Nonnegative trailing spacing in reading order, with zero leading/minimum
// spacing. RTL adjusts the first eligible glyph/offset; LTR adjusts the last.
// Input
// placements and flags are validated shaping output. Empty flags mean one
// independent nominal glyph per cluster. Flags do not overlap placements.
// Mutates only advances/advance offsets;
// no allocation or storage kept between calls. Do not use the result of a failed attempt.
SfntStatus sfnt_trailing_spacing(Slice<SfntPlacement> placements,
    ConstSlice<u8> flags,float trailing_spacing,u64 work_limit=1000000,bool rtl=false) noexcept;
// Read-only finalization compensates removed mark advances within each cluster,
// then converts to binary32 font units and multiplies by binary32 em/upem once.
// No rounding-to-pixel or raster policy is implied. Output valid only on success;
// positions are never changed. Empty runs are allowed. No hidden allocation.
// The Greek/Cyrillic profile compensates consecutive zero-advance
// glyphs across source-cluster boundaries and resets at each advancing glyph.
// Select that policy for those scripts; other script frontends keep their own rule.
SfntStatus sfnt_position_finalize(ConstSlice<SfntPosition> positions,ConstSlice<u8> flags,float em_size,u16 units_per_em,
                                    Slice<SfntPlacement> output,u64 work_limit=1000000,bool consecutive_zeroes=false,bool retain_zero_positioning=false) noexcept;
}
