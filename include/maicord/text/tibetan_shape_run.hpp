#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Experimental itemized Tibetan scalar run. Early GSUB and reorder are local
// to original clusters; isolated/presentation GSUB and GPOS see the whole run.
// No allocation. Mapping borrows positions, boundaries borrow targets, and
// groups borrow character_glyphs until export. Input may borrow the exact
// component-buffer prefix; other input/font views must be disjoint from scratch.
// A bounded original-order selector history lives only for this call. Failures
// publish no output lengths. This does not establish font/app admission.
SfntStatus sfnt_tibetan_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false) noexcept;
}
