#pragma once
#include "maicord/text/shaping/shape_run.hpp"

namespace maicord {
// Shapes a run already resolved to Arabic right to left, as HarfBuzz's Arabic shaper does.
// Scalars align one-to-one with the initial mapped glyphs; callers supply joining forms,
// classes, sources, initial (grapheme) clusters and the separately prepared font plan.
// Cmap and plan belong to the same font; initial source indices are nondecreasing.
// Marks take HarfBuzz's Arabic order (sfnt_arabic_mark_order) before substitution; a
// mark that begins the paragraph gets a dotted circle in its cluster; default ignorables
// that substitution leaves alone become zero-advance space glyphs.
// Storage is reused, not allocated: the character map not yet in use is permutation
// scratch and must fit input.count; an inserted circle borrows the substitution
// replacement scratch until the common coordinator copies the initial glyphs, so glyph,
// component and replacement capacity must cover it; advances hold two bytes per source
// unit until glyph policy completes; scalars may borrow the component buffer prefix, as
// their setup finishes before component history is used.
// Kept separate so unmasked callers do not pull Arabic Unicode preparation into
// their binaries.
SfntStatus sfnt_arabic_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntClusterGlyph> input,ConstSlice<u32> scalars,
    u32 source_units,ConstSlice<u32> cluster_boundaries,bool paragraph_start,float em_size,const SfntShapeStorage& storage,
    SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits={}) noexcept;
} // namespace maicord
