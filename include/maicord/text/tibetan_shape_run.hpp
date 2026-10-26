#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Itemized Tibetan scalar run. Early GSUB and reorder are local
// to original clusters; isolated/presentation GSUB and GPOS see the whole run.
// No allocation. Mapping borrows positions, boundaries borrow targets, and
// groups borrow character_glyphs until export. Input may borrow the exact
// component-buffer prefix; other input/font views must be disjoint from scratch.
// Failures publish no output lengths. Font support is not checked.
SfntStatus sfnt_tibetan_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,const SfntShapeStorage&,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,const SfntShapeLimits& limits={},bool rtl=false) noexcept;
}
