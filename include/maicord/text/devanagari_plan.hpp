#pragma once
#include "maicord/text/arabic_plan.hpp"
namespace maicord {
// Modern Devanagari (dev2), en-US. This schedules font features; the script
// frontend must still classify syllables/forms and set glyph eligibility.
// Presence of dev2 alone does not show the font supports the script. Checking
// font support and detecting forms from the font is the frontend's job.
enum class SfntDevanagariStage : u8 {
    localized,nukta,akhand,reph,rakar,pre_base,below_base,half,post_base,vatu,
    conjunct,final_reorder,presentation,count
};
enum SfntDevanagariForm : u8 {
    sfnt_deva_nukta=1,sfnt_deva_reph=2,sfnt_deva_below=4,sfnt_deva_half=8,
    sfnt_deva_pre=16,sfnt_deva_post=32,sfnt_deva_vatu=64,sfnt_deva_conjunct=128
};
// Only dev2 is selected; legacy deva needs a separate profile. Language
// selection is ENG then DefaultLangSys. Both typography modes keep the required
// Indic features, calt/rclt and kern; no ccmp/liga/clig/rlig/rvrn or
// discretionary features are added.
// GSUB reuses lookups across stages, deduplicates within a stage and reserves an
// empty final-reorder stage. Required features join every executable stage with
// that stage's eligibility. GPOS is one unmasked stage.
// Scratch: lookup_count u16 cells. Output: four bytes per scheduled lookup.
// Views are borrowed and disjoint. Work is shared across selection, counting and
// emission. Short output reports capacity; any failure leaves output untouched.
SfntStatus sfnt_devanagari_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,bool independent,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
// Context-free font classification: locl plus exactly one of reph/half/pre/
// below/post, merged in font lookup order into one unmasked stage. Absent form
// features produce no roots (locl alone cannot establish form availability).
// Required features unrelated to these tags are not classification probes.
// The same storage, selection, variation and all-or-nothing output rules apply.
SfntStatus sfnt_devanagari_form_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,SfntDevanagariForm,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
}
