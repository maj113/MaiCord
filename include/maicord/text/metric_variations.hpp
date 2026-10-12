#pragma once
#include "maicord/text/variation_store.hpp"

namespace maicord {
struct SfntHvar {
    ItemVariationStore store;
    VariationIndexMap advances;
};
struct SfntMvar {
    ItemVariationStore store;
    ConstSlice<u8> records;
    u16 record_stride, record_count;
};
// Borrowed table views. No allocation. Open calls use one shared work
// budget including child-store/map validation. Side-bearing maps are not used by
// the advance-only HVAR reader; ink bearings belong to resolved outlines.
SfntStatus sfnt_hvar_open(ConstSlice<u8> table,SfntHvar* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus sfnt_mvar_open(ConstSlice<u8> table,SfntMvar* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Prepare scalars using variation_instance_prepare(view.store,...). Values are
// unrounded design-unit deltas; the caller applies the metric-specific policy.
// A missing HVAR table must not be interpreted as constant advances in a gvar
// font: it requires phantom-point resolution. Missing MVAR tags contribute zero.
SfntStatus sfnt_hvar_advance_delta(const SfntHvar& view,ConstSlice<double> scalars,u16 glyph,double* output) noexcept;
// Windows design-metric compatibility: compressed hmtx tails share the
// last long metric's HVAR index; round to nearest with ties toward +infinity and
// keep the unsigned 16-bit field. Raw OpenType per-glyph deltas remain above.
// This does not establish final GetGlyphPlacements/raster/layout equivalence.
SfntStatus sfnt_hvar_design_advance(const SfntHvar& view,ConstSlice<double> scalars,const SfntHorizontalMetrics& metrics,u16 glyph,u16* output) noexcept;
SfntStatus sfnt_mvar_delta(const SfntMvar& view,ConstSlice<double> scalars,u32 tag,double* output) noexcept;
} // namespace maicord
