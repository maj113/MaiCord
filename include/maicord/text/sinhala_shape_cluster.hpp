#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// One original Sinhala cluster; input is validated before expansion. The caller
// supplies a prepared Sinhala USE schedule and font. Failed attempts publish no
// output lengths. Input, font, program, result and mutable storage views must be
// disjoint. Position scratch is borrowed for feature-change snapshots until GPOS
// starts. Presentation/GPOS is not applied across multiple original clusters.
SfntStatus sfnt_sinhala_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool rtl=false,bool defer_output=false) noexcept;
}
