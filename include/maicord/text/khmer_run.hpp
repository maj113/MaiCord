#pragma once
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// Borrowed schedule over a retained native instance. Parent and root arena must
// remain live. Cache identity includes slot/generation and typography mode.
struct NativeKhmerPlan {
    NativeRunPlanHandle run;
    const SfntCmap* cmap;
    union {const SfntLookupStep* substitution_steps;const u16* fallback_roots;};
    const u16* positioning_roots;
    u32 substitution_count;
    // GPOS has one deduplicated stage, bounded by the font's u16 lookup count.
    u16 positioning_count;
    bool independent_letters,admitted;
    ConstSlice<SfntLookupStep> substitutions() const noexcept {return admitted?ConstSlice<SfntLookupStep>{substitution_steps,substitution_count}:ConstSlice<SfntLookupStep>{};}
    ConstSlice<u16> positioning() const noexcept {return {positioning_roots,positioning_count};}
    const SfntGposProgram* positioning_program=nullptr;
};
static_assert(sizeof(NativeKhmerPlan)==48);
// No extra retain or font graph. Scratch always rewinds; failed preparation
// rewinds state and clears output. State/scratch and parent arenas are disjoint.
SfntStatus native_khmer_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,bool independent_letters,
    Arena* state,Arena* scratch,NativeKhmerPlan*,u32 stage_work=1000000) noexcept;
// Already-itemized UTF-8. Scalars borrow component scratch until mapped input
// is ready. Source maps use UTF-16. No allocation. Stale handles reject before
// reading borrowed views; every failure leaves output lengths zero.
SfntStatus native_khmer_run(const NativeRunPlanStore&,const NativeKhmerPlan&,Utf8View,float,
    SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
// Compact output-prefix aliases follow native_ascii_run. Missing glyphs reject
// compact publication so the caller can perform font fallback.
SfntStatus native_khmer_run(const NativeRunPlanStore&,const NativeKhmerPlan&,Utf8View,float,
    const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
