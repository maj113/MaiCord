#pragma once
#include "maicord/text/indic_run_plan.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/ascii_run.hpp"
namespace maicord {
using NativeMalayalamPlan=NativeIndicFormPlan;
SfntStatus native_malayalam_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeMalayalamPlan*,SfntShapeLimits limits={}) noexcept;
// Original clusters use bounded decode scratch and one run-wide GPOS pass.
// Font/input views are disjoint from mutable output. No per-call allocation.
SfntStatus native_malayalam_run(const SfntShapePlan&,const SfntCmap&,Utf8View,ConstSlice<u8> forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
SfntStatus native_malayalam_run(const NativeRunPlanStore&,const NativeMalayalamPlan&,Utf8View,
    float,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
// Exact rich-buffer prefixes can hold compact output. Missing glyphs reject
// the complete result; failures publish no output lengths.
SfntStatus native_malayalam_run(const NativeRunPlanStore&,const NativeMalayalamPlan&,Utf8View,
    float,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
