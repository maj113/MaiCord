#pragma once
#include "maicord/text/indic_run_plan.hpp"
#include "maicord/text/base.hpp"
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// Roots/metrics/cmap borrow the retained parent plan. Only roots and the
// 128-byte font-form classification are script-specific; no second font graph.
using NativeBengaliPlan=NativeIndicFormPlan;
static_assert(sizeof(NativeBengaliPlan)==184);
SfntStatus native_bengali_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeBengaliPlan*,SfntShapeLimits limits={}) noexcept;
// Validates UTF-8 before publishing anything; decode window is at most 32
// scalars, independent of run length. All text/font/result views are disjoint
// from mutable caller storage. No allocation or system shaping call.
SfntStatus native_bengali_run(const SfntShapePlan&,const SfntCmap&,Utf8View,ConstSlice<u8> forms,
    float,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
SfntStatus native_bengali_run(const NativeRunPlanStore&,const NativeBengaliPlan&,Utf8View,
    float,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
// Compact output can share exact prefixes of the corresponding rich buffers.
// Missing glyphs reject the whole result for caller fallback; no partial lengths
// are published. Partial aliases and overlaps between different arrays are invalid.
SfntStatus native_bengali_run(const NativeRunPlanStore&,const NativeBengaliPlan&,Utf8View,
    float,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
