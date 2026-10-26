#pragma once
#include "maicord/text/run_plan.hpp"
#include "maicord/text/indic_plan.hpp"
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
};
static_assert(sizeof(NativeIndicPlan)==48);
struct NativeIndicFormPlan {NativeIndicPlan indic;u8 forms[128];};
static_assert(sizeof(NativeIndicFormPlan)==176);
// Appends roots and any required GPOS subset; shares the parent's GSUB, coordinates,
// mapping and metrics, with no extra parent reference or retained arena. Scratch
// rewinds on every return; failed calls also rewind state and clear output. The
// optional 128-byte form output covers the selected script's BMP block and is written
// only on success; without it, masked GSUB steps are rejected. All form probes share
// one lookup budget. Only probe.glyphs/substitutions are used; state, scratch,
// borrowed views and outputs must otherwise be disjoint.
SfntStatus native_indic_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,SfntIndicProfile,
    Arena* state,Arena* scratch,const SfntShapeStorage& probe,NativeIndicPlan*,Slice<u8> forms={},const SfntShapeLimits& limits={}) noexcept;
}
