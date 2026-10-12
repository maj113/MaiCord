#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// One original Khmer cluster with an admitted, prepared font and Khmer plan.
// Native input preparation, initial reordering, GSUB/GPOS and source export.
// No allocation or system shaping. Position scratch holds temporary mapped
// input; flags hold temporary scalar boundaries. Failure publishes no lengths.
// Original input/font/program/result and all mutable storage views are disjoint.
// This API does not apply presentation/GPOS across multiple original clusters.
SfntStatus sfnt_khmer_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool rtl=false,bool defer_output=false) noexcept;
}
