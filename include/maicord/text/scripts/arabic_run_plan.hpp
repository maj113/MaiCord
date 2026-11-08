#pragma once
#include "maicord/text/shaping/run_plan.hpp"

namespace maicord {
struct NativeArabicPlanHandle {u32 slot,generation;};
struct NativeArabicPlanSlot {NativeRunPlanHandle run;Arena state;u32 references,generation;};
struct NativeArabicPlanStore {NativeRunPlanStore* runs;Slice<NativeArabicPlanSlot> slots;};
struct NativeArabicPlanView {const SfntShapePlan* shape;ConstSlice<SfntLookupStep> steps;const SfntCmap* cmap;};
// On-demand Arabic/en-US schedules built on an existing native run plan. Both
// typography modes share its parsed GSUB/GPOS graphs, metric/variation context
// and mapped font; only the Arabic roots/steps and a shape descriptor per mode are
// new, and the modes share positioning unless independent letters drop kerning.
// A live Arabic plan holds one run-plan reference and is read-only once built;
// views stay valid while the plan is retained. Slots are zero-initialized,
// stable and mutated on their owning thread. Handles are store-local.
// Missing usable GSUB script/language support (arab, DFLT, dflt or latn) returns
// unsupported_sequence. Variation feature selection uses the instance's normalized
// coordinates. Both modes are prepared together or not at all.
// Scratch is disposable and reset during preparation. A matching existing plan
// is reused without allocating. Work limits each parser/planner pass
// separately; state_limit includes page rounding.
NativeFontResult native_arabic_plan_acquire(NativeArabicPlanStore* store,NativeRunPlanHandle run,
    Arena* scratch,NativeArabicPlanHandle* output,u32 stage_work=1000000,usize state_limit=1024*1024) noexcept;
NativeArabicPlanView native_arabic_plan_get(const NativeArabicPlanStore& store,NativeArabicPlanHandle handle,bool independent_letters=false) noexcept;
bool native_arabic_plan_release(NativeArabicPlanStore* store,NativeArabicPlanHandle handle) noexcept;
static_assert(sizeof(NativeArabicPlanHandle)==8);
}
