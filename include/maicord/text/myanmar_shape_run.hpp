#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Already-itemized horizontal modern Myanmar run for a font whose program supports Myanmar.
// Early GSUB is cluster-local; isolated/presentation GSUB and GPOS see the run.
// No allocation. Prepared input maps in place in position scratch; targets and
// character_glyphs temporarily hold original source groups. Input may borrow
// the component prefix until mapping completes; other views are disjoint.
// Preparation sees the whole run, including truncated kinzi before punctuation.
// modern=false keeps source clusters/stages without reordering or circles.
// Failure leaves output lengths unset; scratch contents can change.
SfntStatus sfnt_myanmar_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false,bool modern=true) noexcept;
}
