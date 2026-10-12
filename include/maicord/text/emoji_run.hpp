#pragma once
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// A borrowed profile over a retained native run plan. The caller keeps that
// store-local run alive and the root arena unchanged throughout use. Copies do
// not acquire references. GPOS reuses the parent subset or appends a Common
// subset to caller state. Font tables, GSUB and metrics remain shared.
struct NativeEmojiPlan {
    NativeRunPlanHandle run;
    const SfntCmap* cmap;
    ConstSlice<u16> substitution_roots,positioning_roots;
    u16 space_glyph;
    const SfntGposProgram* positioning_program=nullptr;
};
static_assert(sizeof(NativeEmojiPlan)==64);
// Append the Common/en-US root lists to caller-owned persistent storage; no
// page arena is created and no run reference is kept. Scratch rewinds on
// return; failure also rewinds state (committed capacity can remain) and clears
// output. State, scratch, output and the borrowed parent plan are disjoint,
// except that the output may occupy an already allocated state prefix.
// Variation selection uses the parent's instance. stage_work limits each
// directory/planner stage separately.
SfntStatus native_emoji_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,
    Arena* state,Arena* scratch,NativeEmojiPlan* output,u32 stage_work=1000000) noexcept;
// One already selected horizontal Common/emoji item. The caller supplies the
// resolved direction; glyphs and source maps remain in logical order. Font
// selection, bidi, presentation policy and non-emoji shaping are the caller's.
// Controls other than join controls and variation selectors, including emoji
// tag suffixes, return unsupported_sequence for itemization.
// Cmap misses stay zero unless original-source coverage is requested; join
// controls do not require nominal coverage. One following variation selector
// is consumed with its base; an unsupported UVS mapping keeps the nominal base.
// arabic_join_controls preserves the resolved Arabic item's join-control
// source boundaries and suppression policy in the emoji face, independent of
// bidi direction (an Arabic-context emoji can have an even level). It does not
// change the Common emoji feature roots.
// Independent non-mark inputs use nominal metrics only after proving that no
// selected GSUB/GPOS root covers any input glyph; uncertain cases keep full
// shaping. Both paths fill shaping.flags. Coverage proofs have a separate
// stage_work allowance.
// The shaping scratch holds all provenance and cluster state; no allocation.
// Storage, output aliasing and result commit rules follow native_ascii_run; the
// advances prefix also holds one provenance byte per source unit until metrics.
SfntStatus native_emoji_run(const NativeRunPlanStore&,const NativeEmojiPlan&,Utf8View,
    float em_size,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool require_source_coverage=false,bool right_to_left=false,bool arabic_join_controls=false) noexcept;
}
