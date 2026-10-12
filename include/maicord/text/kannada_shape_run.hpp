#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Complete original clusters share a bounded decode window and one run-wide
// positioning pass. Source maps and resolved owners outlive component scratch.
SfntStatus sfnt_kannada_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,ConstSlice<u8> forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
