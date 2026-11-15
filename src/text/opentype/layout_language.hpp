#pragma once
#include "maicord/text/scripts/arabic_plan.hpp"
namespace maicord {
// Internal planner boundary: the caller already checked the prepared layout,
// candidate slice and result storage. Selection is zero initialized; work is a
// disjoint local counter. Keep directory/content and work-limit checks in the
// shared walk, but avoid repeating argument checks in each prepared profile.
SfntStatus sfnt_layout_select_language_prepared(const SfntLayout&,ConstSlice<u32>,u32 requested_language,
    SfntLanguageSelection&,u64& remaining_work) noexcept;
// A feature of a staged lookup plan. Its lookups gather mask bits in the scratch: a feature without a glyph mask
// applies to every glyph (unmasked), a global feature applies across syllables.
inline constexpr u16 sfnt_plan_unmasked=256,sfnt_plan_global=512;
struct SfntStagedFeature {SfntFeature feature;u8 stage,mask;bool global;};
// Publishes the lookups of the features stage by stage: a count pass, the checks on the output storage, then a
// write pass. Stage `skipped` publishes nothing and a later stage publishes phase 1 (the Indic plan). Without a
// skipped stage (255) the phase is the stage.
SfntStatus sfnt_staged_lookup_plan(const SfntLayout&,ConstSlice<SfntStagedFeature>,u32 stages,u32 skipped,Slice<u16> scratch,
    Slice<SfntLookupStep> output,Slice<u64> per_syllable,SfntLookupPlanResult& result,u64& work) noexcept;
}
