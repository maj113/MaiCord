#pragma once
#include "maicord/text/font_instance.hpp"
#include "maicord/text/shape_run.hpp"
#include "maicord/text/font_metrics.hpp"

namespace maicord {
enum class NativeRunPlanKind:u8 {text,emoji};
struct NativeRunPlanHandle {u32 slot,generation;};
struct NativeRunPlanSlot {
    NativeFontInstanceHandle instance;
    Arena state;
    u32 references,generation;
};
struct NativeRunPlanStore {NativeFontInstanceStore* instances;Slice<NativeRunPlanSlot> slots;Arena* substitutions=nullptr;};
// The optional affine cmap prefix covers [U+0020, U+0020 + ascii_count).
// Both fields fit in the tail padding; the borrowed result stays 24 bytes.
struct NativeNominalPlan {ConstSlice<u64> participation;u32 glyph_limit;u16 ascii_first,ascii_count;};
static_assert(sizeof(NativeNominalPlan)==24);
// Text plans prepare Latin/en-US roots and nominal participation. Emoji plans
// prepare Common roots for native_emoji_run only. GSUB payloads are loaded lazily.
// Both typography modes share GSUB, reachable GPOS lookups and the metric
// context. This does not implement TextLayout's nominal range gate,
// script selection, fallback or script-specific stage/mask/reordering policy.
// Slots are stable, zero initialized and mutated on the owning thread. A live
// plan keeps its instance alive. Borrowed plan pointers stay valid while the plan is held.
// Scratch is disposable, disjoint from live state and reset on acquisition.
// Base state is read-only. A shared GSUB directory, grown on the owning thread on first
// lookup use, never moves or changes committed payloads. The limit covers base
// committed bytes plus lookup bytes. The pool is released with the last plan.
// Work bounds each parser/planner stage; it is not a total CPU-time allowance.
// Oblique reuses the base plan. Static TrueType bold keeps a one-page metric
// view over that plan's programs, roots and nominal data. The limit
// includes newly prepared base state and the view; cached state is reused.
NativeFontResult native_run_plan_acquire(NativeRunPlanStore* store,NativeFontInstanceHandle instance,
    Arena* scratch,NativeRunPlanHandle* output,u32 stage_work=1000000,usize state_limit=16*1024*1024,u32 simulations=0,NativeRunPlanKind kind=NativeRunPlanKind::text) noexcept;
const SfntShapePlan* native_run_plan_get(const NativeRunPlanStore& store,NativeRunPlanHandle handle,
    bool independent_letters=false) noexcept;
// Emoji-only plans have no nominal mapping and return 0 limit.
// Base-feature participation is shared by both modes and all ranges in this
// instance. Empty bits mean no participating glyphs; invalid handles return 0 limit.
NativeNominalPlan native_run_plan_nominal(const NativeRunPlanStore& store,NativeRunPlanHandle handle) noexcept;
const SfntFontLayoutMetrics* native_run_plan_metrics(const NativeRunPlanStore& store,NativeRunPlanHandle handle) noexcept;
bool native_run_plan_retain(NativeRunPlanStore* store,NativeRunPlanHandle handle) noexcept;
bool native_run_plan_release(NativeRunPlanStore* store,NativeRunPlanHandle handle) noexcept;
static_assert(sizeof(NativeRunPlanHandle)==8);
}
