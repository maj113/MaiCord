#pragma once
#include "maicord/text/indic_run_plan.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/ascii_run.hpp"
namespace maicord {
using NativeOdiaPlan=NativeIndicFormPlan;
SfntStatus native_odia_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeOdiaPlan*,SfntShapeLimits limits={}) noexcept;
// No allocation. Font/input views are disjoint from mutable output/scratch.
SfntStatus native_odia_run(const SfntShapePlan&,const SfntCmap&,Utf8View,ConstSlice<u8> forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
SfntStatus native_odia_run(const NativeRunPlanStore&,const NativeOdiaPlan&,Utf8View,
    float,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
// Compact output accepts exact prefixes of corresponding rich buffers.
// Missing glyphs reject the complete result. Failures publish no lengths.
SfntStatus native_odia_run(const NativeRunPlanStore&,const NativeOdiaPlan&,Utf8View,
    float,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
