#pragma once
#include "maicord/text/indic_run_plan.hpp"
#include "maicord/text/base.hpp"
#include "maicord/text/ascii_run.hpp"
namespace maicord {
using NativeGurmukhiPlan=NativeIndicFormPlan;
SfntStatus native_gurmukhi_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeGurmukhiPlan*,SfntShapeLimits limits={}) noexcept;
// Prevalidated UTF-8, bounded decode window, cluster GSUB and one run-wide GPOS
// pass. No allocation. Font/input views are disjoint from mutable output/scratch.
SfntStatus native_gurmukhi_run(const SfntShapePlan&,const SfntCmap&,Utf8View,ConstSlice<u8> forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
SfntStatus native_gurmukhi_run(const NativeRunPlanStore&,const NativeGurmukhiPlan&,Utf8View,
    float,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
// Compact output may reuse exact prefixes of the corresponding rich buffers.
// Missing glyphs reject the complete compact result; failures publish no lengths.
SfntStatus native_gurmukhi_run(const NativeRunPlanStore&,const NativeGurmukhiPlan&,Utf8View,
    float,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
