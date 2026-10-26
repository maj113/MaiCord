#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Already-itemized Sinhala scalar run. Early substitutions and reordering are
// cluster-local; presentation GSUB and GPOS see the whole run. No allocation.
// Mapped glyphs borrow positions, boundaries borrow targets and source-group IDs
// borrow character_glyphs until final export. Original scalars may borrow the
// exact component-buffer prefix until mapping completes; other input/font views
// must be disjoint from mutable scratch. Failures publish no lengths.
SfntStatus sfnt_sinhala_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,const SfntShapeStorage&,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,const SfntShapeLimits& limits={},bool rtl=false) noexcept;
}
