#pragma once
#include "maicord/text/indic_run_plan.hpp"
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// Already itemized Tamil UTF-8 in one selected font, default typography.
// Scalars borrow the component-buffer prefix until input preparation completes;
// no additional per-character array, allocation or OS text call is required.
// Source maps remain UTF-16 indexed, including both supplementary-code-unit slots.
// Direction controls mirroring/positioning; tracking follows final clusters.
// Remaining script, font, bidi and typography limits are those of the scalar
// Tamil run executor. Missing nominal glyphs remain zero for caller fallback.
SfntStatus native_tamil_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntLookupStep>,
    Utf8View,float em_size,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},float trailing_spacing=0,bool rtl=false) noexcept;
// Reject a stale parent or wrong script identity before dereferencing views.
SfntStatus native_tamil_run(const NativeRunPlanStore&,const NativeIndicPlan&,Utf8View,
    float em_size,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},float trailing_spacing=0,bool rtl=false) noexcept;
// Compact publication accepts exact-prefix aliases of its corresponding rich
// buffers, as in native_latin_run. Missing glyphs reject the complete result;
// failures publish no lengths. Partial aliases and other overlaps are invalid.
SfntStatus native_tamil_run(const NativeRunPlanStore&,const NativeIndicPlan&,Utf8View,
    float em_size,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},float trailing_spacing=0,bool rtl=false) noexcept;
}
