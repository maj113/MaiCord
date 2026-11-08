#pragma once
#include "maicord/text/scripts/ascii_run.hpp"
#include "maicord/text/shaping/alphabetic_run.hpp"
namespace maicord {
// The scripts that share one cluster-run frontend and plan layout. Sinhala,
// Khmer, Myanmar and Tibetan are consecutive so frontends can map script ids;
// the order is SfntSyllabicScript's.
enum class NativeClusterScript : u8 { sinhala,khmer,myanmar,tibetan };
// Borrowed en-US schedule over one retained native instance plan. Cache
// identity includes parent slot/generation, script and independent-letter mode.
// The caller retains the parent and root arena for this view's entire lifetime.
// HarfBuzz picks the shaper from the font's GSUB script (hb_ot_shaper_categorize):
// Khmer always uses the Khmer shaper; a Myanmar font whose GSUB selects DFLT, latn
// or mymr, and a Sinhala or Tibetan font whose GSUB selects DFLT or latn, use the
// default shaper (fallback, the Common alphabetic plan); every other font uses
// the script's shaper (sfnt_syllabic_run) with these steps.
struct NativeClusterPlan {
    NativeRunPlanHandle run;
    const SfntCmap* cmap;
    const SfntLookupStep* substitution_steps;
    const u16* positioning_roots;
    // Bit i: substitution step i runs within each syllable.
    const u64* per_syllable_steps;
    // The GSUB lookups of the shaper's manual-joiner features (SfntShapePlan substitution_manual); null when none.
    const u64* substitution_manual;
    const NativeAlphabeticPlan* fallback;
    u32 substitution_count;
    // GPOS has one deduplicated stage, bounded by the font's u16 lookup count.
    u16 positioning_count;
    bool independent_letters;
    ConstSlice<SfntLookupStep> substitutions() const noexcept {return {substitution_steps,substitution_count};}
    ConstSlice<u16> positioning() const noexcept {return {positioning_roots,positioning_count};}
    ConstSlice<u64> per_syllable() const noexcept {return {per_syllable_steps,per_syllable_steps?(substitution_count+63)/64:0u};}
    const SfntGposProgram* positioning_program=nullptr;
};
static_assert(sizeof(NativeClusterPlan)==72);
// Shares immutable font graphs, metrics and instance coordinates. State holds
// selected steps and any missing positioning records; existing parent records
// stay shared. Scratch always rewinds; failed preparation rewinds state and
// clears output. State/scratch/output and retained parent arenas must be disjoint.
SfntStatus native_cluster_plan_prepare(NativeClusterScript,const NativeRunPlanStore&,NativeRunPlanHandle,bool independent_letters,
    Arena* state,Arena* scratch,NativeClusterPlan*,u32 stage_work=1000000) noexcept;
// Already-itemized UTF-8 in the script's shaper. Output source maps use UTF-16
// units. No allocation. Stale handles reject before borrowed views are read;
// every failure leaves output lengths zero. A plan that falls back to the
// default shaper shapes only through the compact overload (unsupported_sequence here).
SfntStatus native_cluster_run(NativeClusterScript,const NativeRunPlanStore&,const NativeClusterPlan&,const Utf8View&,
    float,const SfntShapeStorage&,SfntShapeRunResult*,const SfntShapeLimits& limits={},bool rtl=false,float trailing_spacing=0) noexcept;
// Exact corresponding output-prefix aliases are permitted as in native_ascii_run.
// Missing final glyph IDs reject compact publication so the caller can fall
// back to another font. Failures publish no output lengths.
SfntStatus native_cluster_run(NativeClusterScript,const NativeRunPlanStore&,const NativeClusterPlan&,const Utf8View&,
    float,const NativeAsciiStorage&,NativeAsciiResult*,const SfntShapeLimits& limits={},bool rtl=false,float trailing_spacing=0) noexcept;
}
