#pragma once
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// Borrowed Sinhala/en-US schedule over one retained native instance plan.
// Cache identity includes parent slot/generation and independent-letter mode.
// The caller retains the parent and root arena for this view's entire lifetime.
struct NativeSinhalaPlan {
    NativeRunPlanHandle run;
    const SfntCmap* cmap;
    const SfntLookupStep* substitution_steps;
    const u16* positioning_roots;
    u32 substitution_count;
    // GPOS has one deduplicated stage, bounded by the font's u16 lookup count.
    u16 positioning_count;
    bool independent_letters,language_found;
    ConstSlice<SfntLookupStep> substitutions() const noexcept {return {substitution_steps,substitution_count};}
    ConstSlice<u16> positioning() const noexcept {return {positioning_roots,positioning_count};}
    const SfntGposProgram* positioning_program=nullptr;
};
static_assert(sizeof(NativeSinhalaPlan)==48);
// Shares immutable font graphs, metrics and instance coordinates. State holds
// selected roots and any missing positioning records; existing parent records
// stay shared. Scratch always rewinds; failed preparation rewinds state and
// clears output. No extra parent retain or consonant-form probes.
// State/scratch/output and retained parent arenas must be disjoint.
SfntStatus native_sinhala_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,bool independent_letters,
    Arena* state,Arena* scratch,NativeSinhalaPlan*,u32 stage_work=1000000) noexcept;
// Already-itemized UTF-8; scalar input borrows the component-buffer prefix
// until mapped input is ready. Output source maps use UTF-16 units. No allocation.
SfntStatus native_sinhala_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntLookupStep>,Utf8View,
    float,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0,bool use_shaping=true) noexcept;
// Reject stale handles before accessing borrowed font/root views.
SfntStatus native_sinhala_run(const NativeRunPlanStore&,const NativeSinhalaPlan&,Utf8View,
    float,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
// Exact corresponding output-prefix aliases are permitted as in native_ascii_run.
// Missing glyphs reject compact publication; failures publish no output lengths.
SfntStatus native_sinhala_run(const NativeRunPlanStore&,const NativeSinhalaPlan&,Utf8View,
    float,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
