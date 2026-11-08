#pragma once
#include "maicord/text/shaping/run_plan.hpp"
#include "maicord/text/scripts/indic_plan.hpp"
namespace maicord {
// Borrowed script-specific roots over a retained native font-instance plan.
// A cache key must include run slot/generation AND profile; the same font can
// provide different modern Indic programs. The owner retains run and its arena.
struct NativeIndicPlan {
    NativeRunPlanHandle run;
    const SfntCmap* cmap;
    const SfntLookupStep* substitution_steps;
    const u16* positioning_roots;
    u32 substitution_count;
    // GPOS has one deduplicated stage, bounded by the font's u16 lookup count.
    u16 positioning_count;
    SfntIndicProfile profile;
    ConstSlice<SfntLookupStep> substitutions() const noexcept {return {substitution_steps,substitution_count};}
    ConstSlice<u16> positioning() const noexcept {return {positioning_roots,positioning_count};}
    const SfntGposProgram* positioning_program=nullptr;
    // Bit i: substitution step i runs within each syllable (sfnt_indic_lookup_plan).
    const u64* per_syllable_steps=nullptr;
    ConstSlice<u64> per_syllable() const noexcept {return {per_syllable_steps,(substitution_count+63)/64};}
    // The GSUB lookups of HarfBuzz's manual-joiner features (SfntShapePlan substitution_manual: two bits per
    // lookup index of the font's GSUB); null when none.
    const u64* substitution_manual=nullptr;
};
static_assert(sizeof(NativeIndicPlan)==64);
struct NativeIndicFormPlan {NativeIndicPlan indic;u8 forms[128];};
static_assert(sizeof(NativeIndicFormPlan)==192);
// Appends roots and any required GPOS subset; shares the parent's GSUB, coordinates,
// mapping and metrics, with no extra parent reference or retained arena. Scratch
// rewinds on every return; failed calls also rewind state and clear output. The
// optional 128-byte form output covers the selected script's BMP block (sfnt_deva_*
// would-substitute bits per consonant, the font forms an Indic run needs) and is written
// only on success. All form queries share one lookup budget. State, scratch, borrowed
// views and outputs must be disjoint.
SfntStatus native_indic_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,SfntIndicProfile,
    Arena* state,Arena* scratch,NativeIndicPlan*,Slice<u8> forms={},const SfntShapeLimits& limits={}) noexcept;
}
