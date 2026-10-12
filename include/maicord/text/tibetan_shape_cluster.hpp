#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// One original Tibetan cluster, at most 62 scalars including
// attached selectors; a longer input or more than one cluster returns
// unsupported_sequence. The caller provides the prepared Tibetan schedule and
// font. No allocation. Failures publish no output lengths. All views are
// disjoint; positions are borrowed first for initial mapping, then for
// feature-change snapshots.
SfntStatus sfnt_tibetan_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool rtl=false,bool defer_output=false) noexcept;
}
