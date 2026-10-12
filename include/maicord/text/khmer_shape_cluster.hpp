#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// One original Khmer cluster with an admitted, prepared font and Khmer plan: input
// preparation, initial reordering, GSUB/GPOS and source export. Position scratch
// holds temporary mapped input; flags hold temporary scalar boundaries. Failure
// publishes no lengths. Input, font, program, result and all mutable storage views
// are disjoint. Presentation/GPOS is not applied across multiple original clusters.
SfntStatus sfnt_khmer_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool rtl=false,bool defer_output=false) noexcept;
}
