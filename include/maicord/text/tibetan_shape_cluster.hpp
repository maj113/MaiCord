#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Experimental one original Tibetan cluster, at most 62 scalars including
// attached selectors. Caller provides the prepared Tibetan schedule and font.
// No allocation or system shaping. Failures publish no output lengths. All
// views are disjoint; positions are borrowed first for initial mapping, then
// for feature-change snapshots. Split pairs stream directly into that storage.
// This does not establish font admission, full-run scheduling or app support.
SfntStatus sfnt_tibetan_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool rtl=false,bool defer_output=false) noexcept;
}
