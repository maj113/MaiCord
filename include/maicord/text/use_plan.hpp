#pragma once
#include "maicord/text/arabic_plan.hpp"
namespace maicord {
// Horizontal, en-US Sinhala (sinh), matching Windows USE. This plans
// features only: it does not accept Sinhala text or implement cluster/reordering
// rules. Other USE scripts need their own acceptance and joining policy.
enum class SfntUseStage : u8 { preprocessing,reph,pre_base,orthographic,reorder,isolated,presentation,count };
// Sinhala rphf executes over the complete cluster, as on Windows. Every
// step is unmasked; the generic USE first-three description is not this policy.
// GSUB groups run in font lookup order; shared lookups collapse within a group
// and stay in each group where they appear. Reordering is an empty frontend boundary.
// Sinhala uses isolated forms; the rvrn feature joins the presentation group.
// Independent letters suppress liga/clig/calt and GPOS kern/curs. Required
// features stay active once per executable group, including isolated forms.
// GPOS is one unmasked group. Select sinh only; ENG then DefaultLangSys.
// Scratch: one byte per font lookup, holding all stage memberships. Output:
// four bytes per emitted lookup. No allocation or feature-view array is kept.
// Layout/variations are borrowed and disjoint from storage. Every failure
// leaves output untouched; scratch is disposable. Short output reports count.
SfntStatus sfnt_sinhala_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,bool independent,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
// Horizontal en-US Myanmar schedule for text the frontend has accepted. Uses the same
// six executable USE groups and unmasked whole-cluster rphf as this Sinhala
// profile. Modern GSUB selects mym2 then DFLT; legacy GSUB selects mymr then
// DFLT. GPOS requires a mym2, latn or DFLT script record (no language needed
// for this gate), then selects mym2 for modern or mymr for legacy, followed
// by latn then DFLT. Language selection is ENG then default.
// Modern acceptance requires a default mym2 language in either layout table;
// language_found alone is not the acceptance rule. This does not implement
// syllable boundaries, character reordering, dotted circles or run shaping.
// Storage, work limits, feature variation and all-or-nothing output contracts above
// apply unchanged. Independent letters suppress the same optional features.
SfntStatus sfnt_myanmar_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,bool independent,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000,bool modern=true) noexcept;
// Horizontal en-US Tibetan: same six executable groups, default language only.
// GSUB selects tibt then DFLT; GPOS selects tibt, latn then DFLT. A present
// script without a default language stops fallback. This plans features only;
// Tibetan cluster preparation, text acceptance and run execution are separate.
// The same storage, work, variation and all-or-nothing output contracts apply.
SfntStatus sfnt_tibetan_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,bool independent,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
}
