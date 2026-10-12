#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Scalar form of the shared run coordinator. Original source positions and
// exported component associations survive per-cluster scratch reuse.
SfntStatus sfnt_gurmukhi_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,ConstSlice<u8> forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
