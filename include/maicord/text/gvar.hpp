#pragma once
#include "maicord/text/sfnt.hpp"
namespace maicord {
struct SfntOutlinePoint;
struct SfntTrueType;
struct SfntGvar {
    ConstSlice<u8> table,offsets,shared_tuples;
    u32 data_offset;
    u16 axis_count,glyph_count,shared_count,offset_stride;
};
struct SfntGvarInstance {
    SfntGvar font;
    ConstSlice<double> coordinates,shared_scalars;
};
struct SfntGvarDelta {double x,y;};
struct SfntGvarScratch {
    Slice<u32> shared_points,private_points;
    Slice<i32> x_deltas,y_deltas;
};
// Views are borrowed, storage is caller-owned, work is limited and nothing allocates.
// Open checks the header and array extents in O(1); each glyph's offsets and
// tuple coordinates are validated when that glyph is used.
SfntStatus sfnt_gvar_open(ConstSlice<u8> table,u16 axes,u16 glyphs,SfntGvar* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus sfnt_gvar_instance_prepare(const SfntGvar& font,ConstSlice<double> coordinates,Slice<double> shared_scalars,SfntGvarInstance* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Horizontal phantom metrics for an HVAR-free font. Reads only glyph counts,
// metric-bearing component chains and packed deltas; no outline storage/IUP.
// Tuple extents and required X runs are checked; unused X/Y suffixes are skipped.
// Views must belong to the same font. No partial output on failure.
SfntStatus sfnt_gvar_design_advance(const SfntTrueType&,const SfntGvarInstance&,u16 glyph,u16* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Simple points use original design coordinates; contour ends are local unless
// point_origin gives their offset in a parent's point arena. For composites
// points.data may be null and points.count is the component count (components
// are not interpolated). Four phantom deltas follow the point deltas.
// Arithmetic follows Windows: normalized coordinates use F2DOT14 and each
// inferred tuple contribution truncates toward zero, while explicit deltas stay
// fractional until the outline resolver rounds the final point.
// Scratch/output must not overlap inputs or each other. Output is valid only on
// success; scratch may change on failure.
SfntStatus sfnt_gvar_deltas(const SfntGvarInstance& instance,u16 glyph,ConstSlice<SfntOutlinePoint> points,ConstSlice<u32> ends,bool composite,SfntGvarScratch scratch,Slice<SfntGvarDelta> output,u32 work_limit=1000000,u32* work_used=nullptr,u32 point_origin=0) noexcept;
} // namespace maicord
