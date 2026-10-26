#pragma once
#include "maicord/text/shape_run.hpp"

namespace maicord {
// For a run already resolved to Arabic. Scalars align one-to-one with the initial
// mapped glyphs; callers supply joining forms, classes, sources, initial clusters
// and the separately prepared font plan. Cmap and plan belong to the same font;
// initial source indices are nondecreasing. Each leading combining mark gets a
// dotted circle before substitution, after transient mark ordering; source positions
// stay in logical order. Profile barriers, leading marks and join controls start
// their own initial shaping clusters; join controls become zero-advance space
// glyphs after substitution.
// Storage is reused, not allocated: the character map not yet in use is permutation
// scratch and must fit input.count; expanded initial glyphs borrow the substitution
// replacement scratch until the common coordinator copies them, so glyph, component
// and replacement capacity must cover the expansion; targets hold merged boundaries;
// advances hold one byte per source unit until glyph policy completes; scalars may
// borrow the component buffer prefix, as their setup finishes before component
// history is used.
// Forced-LTR recomputes backward joining eligibility, keeps input mark order, and
// writes logical glyph/maps/flags with LTR placements; other arrays remain scratch.
// CGJ in forced-LTR is unsupported, some control/mark mixtures differ from Windows,
// and other controls and normalization are not implemented.
// Kept separate so unmasked callers do not pull Arabic Unicode preparation into
// their binaries.
SfntStatus sfnt_arabic_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntClusterGlyph> input,ConstSlice<u32> scalars,
    u32 source_units,ConstSlice<u32> cluster_boundaries,bool rtl,float em_size,const SfntShapeStorage& storage,
    SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits={}) noexcept;
} // namespace maicord
