#pragma once
#include "maicord/text/shape_run.hpp"

namespace maicord {
// For a run the caller has already resolved to the Arabic script. Performs Arabic
// mark-order preparation and coordinated lookup execution. Scalars align
// one-to-one with the initial mapped glyphs; callers supply their joining forms,
// classes, sources, initial clusters and the separately prepared font plan.
// The character map not yet in use supplies permutation scratch and must also fit
// input.count. Each leading combining mark gets a dotted circle before
// substitution, after transient mark ordering; source positions stay in logical
// order. Expanded initial glyphs borrow the substitution replacement scratch until
// the common coordinator copies them, so the caller must provide enough
// glyph/component and replacement capacity for this expansion.
// Profile barriers, leading marks and join controls get their own initial
// shaping-cluster starts; join controls become zero-advance space glyphs after
// substitution. The target array supplies temporary merged boundaries and advance
// storage supplies one byte per source unit until glyph policy completes; both are
// reused, not extra allocations.
// Scalars may borrow the component buffer prefix: all scalar-dependent setup
// finishes before the common coordinator starts using component history.
// Cmap and plan must belong to the same font. Initial source indices are nondecreasing.
// Forced-LTR mode recomputes backward joining eligibility, keeps input mark
// order, reverses cluster processing in replacement scratch, then writes
// logical glyph/maps/flags and LTR placements. Other arrays remain scratch.
// Join-control tracking also borrows a second source-unit byte span in advances.
// CGJ in forced-LTR is unsupported and some control/mark mixtures still differ
// from Windows; other controls and normalization are not implemented.
// Kept separate so unmasked callers do not pull
// Arabic Unicode preparation into their binaries.
SfntStatus sfnt_arabic_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntClusterGlyph> input,ConstSlice<u32> scalars,
    u32 source_units,ConstSlice<u32> cluster_boundaries,bool rtl,float em_size,SfntShapeStorage storage,
    SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits={}) noexcept;
} // namespace maicord
