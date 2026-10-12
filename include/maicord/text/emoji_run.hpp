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
// Append the Common/en-US root lists to caller-owned persistent storage. This
// does not create a page arena or keep a reference to the run. Scratch rewinds on return;
// failure also rewinds state and clears output. Committed capacity can remain.
// State, scratch, output and the borrowed parent plan are disjoint. The output
// may occupy an already allocated state prefix. Variation selection uses the
// parent's instance. Work limits each directory/planner stage separately.
SfntStatus native_emoji_plan_prepare(const NativeRunPlanStore&,NativeRunPlanHandle,
    Arena* state,Arena* scratch,NativeEmojiPlan* output,u32 stage_work=1000000) noexcept;
// One already selected horizontal Common/emoji item. Resolved direction is
// supplied by the caller; glyphs and source maps remain in logical order.
// Font selection, paragraph bidi,
// non-visual items, presentation policy and general script shaping are caller
// responsibilities. Controls other than join controls and variation selectors,
// including emoji tag suffixes, return unsupported_sequence for itemization.
// Cmap misses stay zero unless original-source coverage is requested. Join
// controls do not require nominal coverage. One following variation selector
// is consumed with its base; unsupported UVS mappings keep the nominal base.
// Current GSUB components distinguish surviving controls from visible compounds.
// arabic_join_controls preserves the resolved Arabic item's join-control
// source boundaries and suppression policy in the app's emoji face. This is
// independent of bidi direction: an Arabic-context emoji can have an even level.
// It does not change the Common emoji feature roots or add a profile.
// Independent non-mark inputs use nominal metrics only after proving that no
// selected GSUB/GPOS root covers any input glyph. A bounded per-run miss cache
// reuses that proof for repeated glyphs; uncertain cases keep full shaping.
// Both paths fill shaping.flags. Coverage proofs have a separate stage_work
// allowance and need no glyph-participation table kept between runs.
// The shaping scratch holds all provenance and cluster state; no extra per-glyph
// array, allocation, OS call or external library is needed.
// Storage, output aliasing and result commit rules follow native_ascii_run; the advances
// prefix additionally holds one provenance byte per source unit until metrics.
SfntStatus native_emoji_run(const NativeRunPlanStore&,const NativeEmojiPlan&,Utf8View,
    float em_size,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool require_source_coverage=false,bool right_to_left=false,bool arabic_join_controls=false) noexcept;
}
