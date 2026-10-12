#pragma once
#include "maicord/text/run_plan.hpp"

namespace maicord {
struct NativeArabicPlanHandle {u32 slot,generation;};
struct NativeArabicPlanSlot {NativeRunPlanHandle run;Arena state;u32 references,generation;};
struct NativeArabicPlanStore {NativeRunPlanStore* runs;Slice<NativeArabicPlanSlot> slots;};
struct NativeArabicPlanView {const SfntShapePlan* shape;ConstSlice<SfntLookupStep> steps;const SfntCmap* cmap;bool generic=false;};
// On-demand Arabic/en-US schedules built on an existing native run plan. Both
// typography modes share its parsed GSUB/GPOS graphs, metric/variation context
// and mapped font. Only the Arabic roots/steps and one shared shape descriptor
// are new; positioning roots are shared between the two typography modes.
// A live Arabic plan holds one run-plan reference. Its state is read-only once
// built; views stay valid while the plan is retained. Slots are zero-initialized,
// stable and mutated on their owning thread. Handles are store-local.
// Missing usable GSUB script/language support returns unsupported_sequence;
// there is no fallback to the legacy Arabic-font path. GPOS is enabled
// only after that GSUB check succeeds. Variation feature selection uses the
// instance's normalized coordinates. Both modes are prepared together or not at all.
// Scratch is disposable, disjoint from live state, and reset during preparation.
// An existing matching plan is reused without allocating new state. Work
// limits each parser/planner pass separately; state_limit includes page rounding.
NativeFontResult native_arabic_plan_acquire(NativeArabicPlanStore* store,NativeRunPlanHandle run,
    Arena* scratch,NativeArabicPlanHandle* output,u32 stage_work=1000000,usize state_limit=1024*1024) noexcept;
NativeArabicPlanView native_arabic_plan_get(const NativeArabicPlanStore& store,NativeArabicPlanHandle handle,bool independent_letters=false) noexcept;
bool native_arabic_plan_retain(NativeArabicPlanStore* store,NativeArabicPlanHandle handle) noexcept;
bool native_arabic_plan_release(NativeArabicPlanStore* store,NativeArabicPlanHandle handle) noexcept;
static_assert(sizeof(NativeArabicPlanHandle)==8);
}
