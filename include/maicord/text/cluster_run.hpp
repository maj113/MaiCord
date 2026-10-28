#pragma once
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// The scripts that share one cluster-run frontend and plan layout. Sinhala,
// Khmer, Myanmar and Tibetan are consecutive so frontends can map script ids.
enum class NativeClusterScript : u8 { sinhala,khmer,myanmar,tibetan };
// Borrowed en-US schedule over one retained native instance plan. Cache
// identity includes parent slot/generation, script and independent-letter mode.
// The caller retains the parent and root arena for this view's entire lifetime.
// `admitted` is per script: Khmer, a gate that accepted the font (khmr in both
// tables with GDEF); Tibetan, a tibt script record in either table (USE
// admission, independent of languages and GDEF); Myanmar, a default mym2
// language in either table (modern schedule, otherwise legacy mymr); Sinhala, a
// sinh language system. Without it Khmer uses the generic fallback roots, which
// share the steps' storage (`substitutions()` is only valid for admitted Khmer
// plans), and Sinhala/Tibetan use the parent nominal program.
struct NativeClusterPlan {
    NativeRunPlanHandle run;
    const SfntCmap* cmap;
    union {const SfntLookupStep* substitution_steps;const u16* fallback_roots;};
    const u16* positioning_roots;
    u32 substitution_count;
    // GPOS has one deduplicated stage, bounded by the font's u16 lookup count.
    u16 positioning_count;
    bool independent_letters,admitted;
    ConstSlice<SfntLookupStep> substitutions() const noexcept {return {substitution_steps,substitution_count};}
    ConstSlice<u16> positioning() const noexcept {return {positioning_roots,positioning_count};}
    const SfntGposProgram* positioning_program=nullptr;
};
static_assert(sizeof(NativeClusterPlan)==48);
// Shares immutable font graphs, metrics and instance coordinates. State holds
// selected roots and any missing positioning records; existing parent records
// stay shared. Scratch always rewinds; failed preparation rewinds state and
// clears output. No extra parent retain or consonant-form probes.
// State/scratch/output and retained parent arenas must be disjoint.
SfntStatus native_cluster_plan_prepare(NativeClusterScript,const NativeRunPlanStore&,NativeRunPlanHandle,bool independent_letters,
    Arena* state,Arena* scratch,NativeClusterPlan*,u32 stage_work=1000000) noexcept;
// Already-itemized UTF-8; scalar input borrows the component-buffer prefix
// until mapped input is ready. Output source maps use UTF-16 units. No
// allocation. Stale handles reject before borrowed views are read; every
// failure leaves output lengths zero.
SfntStatus native_cluster_run(NativeClusterScript,const NativeRunPlanStore&,const NativeClusterPlan&,const Utf8View&,
    float,const SfntShapeStorage&,SfntShapeRunResult*,const SfntShapeLimits& limits={},bool rtl=false,float trailing_spacing=0) noexcept;
// Exact corresponding output-prefix aliases are permitted as in native_ascii_run.
// Missing final glyph IDs reject compact publication so the caller can fall
// back to another font; control-ID aliasing follows the script's Windows policy.
// Failures publish no output lengths.
SfntStatus native_cluster_run(NativeClusterScript,const NativeRunPlanStore&,const NativeClusterPlan&,const Utf8View&,
    float,const NativeAsciiStorage&,NativeAsciiResult*,const SfntShapeLimits& limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
