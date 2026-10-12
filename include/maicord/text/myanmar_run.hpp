#pragma once
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// Borrowed Myanmar/en-US schedule over one retained native instance plan.
// Cache identity includes parent slot/generation and independent-letter mode.
// The caller retains the parent and root arena for this view's entire lifetime.
struct NativeMyanmarPlan {
    NativeRunPlanHandle run;
    const SfntCmap* cmap;
    const SfntLookupStep* substitution_steps;
    const u16* positioning_roots;
    u32 substitution_count,positioning_count;
    bool independent_letters,modern;
    ConstSlice<SfntLookupStep> substitutions() const noexcept {return {substitution_steps,substitution_count};}
    ConstSlice<u16> positioning() const noexcept {return {positioning_roots,positioning_count};}
    const SfntGposProgram* positioning_program=nullptr;
};
static_assert(sizeof(NativeMyanmarPlan)==56);
// Shares immutable font graphs, metrics and instance coordinates. State holds
// selected roots and any missing positioning records; existing parent records
// stay shared. Scratch always rewinds; failed preparation rewinds state and
// clears output. No extra parent retain or consonant-form probes.
// State/scratch/output and retained parent arenas must be disjoint.
SfntStatus native_myanmar_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,bool independent_letters,
    Arena* state,Arena* scratch,NativeMyanmarPlan*,u32 stage_work=1000000) noexcept;
// Already-itemized UTF-8; scalar input borrows the component-buffer prefix
// until mapped input is ready. Output source maps use UTF-16 units. No allocation.
SfntStatus native_myanmar_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntLookupStep>,Utf8View,
    float,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0,bool modern=true) noexcept;
// Reject stale handles before accessing borrowed font/root views.
SfntStatus native_myanmar_run(const NativeRunPlanStore&,const NativeMyanmarPlan&,Utf8View,
    float,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
// Exact corresponding output-prefix aliases are permitted as in native_ascii_run.
// Missing final glyph IDs reject compact publication; control-ID aliasing follows
// the observed Myanmar policy. Failures publish no output lengths.
SfntStatus native_myanmar_run(const NativeRunPlanStore&,const NativeMyanmarPlan&,Utf8View,
    float,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
