#pragma once
#include "maicord/text/devanagari_run.hpp"

namespace maicord {
// Borrowed modern dev2/en-US profile over a retained native run plan. The
// caller keeps run and its root arena alive. Copies do not acquire references.
// Both typography modes share roots/forms and the selected GPOS subset.
// Parent GSUB, coordinates, metrics and font mapping remain shared.
struct NativeDevanagariPlan {
    NativeRunPlanHandle run;
    const SfntCmap* cmap;
    ConstSlice<SfntLookupStep> substitutions;
    ConstSlice<u16> positioning;
    u8 forms[128];
    const SfntGposProgram* positioning_program=nullptr;
};
static_assert(sizeof(NativeDevanagariPlan)==184);
// Append roots and any required GPOS subset; no second page arena or extra
// run reference. Output may occupy an already allocated state prefix. State,
// scratch, probe arrays, output and font views are otherwise disjoint.
// Only probe.glyphs and probe.substitutions are used. Capacity errors propagate;
// an incomplete form query never becomes a negative form classification.
// Scratch rewinds on every return; failure also rewinds state (committed arena
// capacity may remain) and clears output. The lookup budget is shared by all
// form probes; stage_work bounds each directory/planner pass independently.
// Missing dev2 language or halant returns unsupported_sequence.
SfntStatus native_devanagari_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,
    Arena* state,Arena* scratch,SfntShapeStorage probe,NativeDevanagariPlan*,SfntShapeLimits limits={}) noexcept;
// Reject stale parent handles before using any borrowed profile pointer.
// Run storage and limitations are those of native_devanagari_run.
SfntStatus native_devanagari_run(const NativeRunPlanStore&,const NativeDevanagariPlan&,
    Utf8View,float em_size,NativeDevanagariRunStorage,NativeAsciiResult*,SfntShapeLimits limits={},float trailing_spacing=0,bool rtl=false) noexcept;
}
