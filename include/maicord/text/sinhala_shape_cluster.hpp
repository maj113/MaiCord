#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Experimental single ORIGINAL Sinhala cluster. Input is validated before
// expansion. The caller supplies a prepared Sinhala USE schedule and font.
// No allocation/system shaping. Failed attempts publish no output lengths.
// Input/font/program/result and mutable storage views must be disjoint.
// Position scratch is borrowed for feature-change snapshots until GPOS starts.
// This does not apply presentation/GPOS across multiple original clusters.
SfntStatus sfnt_sinhala_shape_cluster(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},
    bool rtl=false,bool defer_output=false) noexcept;
}
