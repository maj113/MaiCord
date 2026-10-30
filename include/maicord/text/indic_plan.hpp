#pragma once
#include "maicord/text/devanagari_plan.hpp"
namespace maicord {
// Shared modern-Indic feature schedule. A frontend must separately implement
// its syllables, normalization, font capability, masks and reordering before
// accepting text. Selecting a script table does not establish shaping support.
enum class SfntIndicProfile : u8 {devanagari,tamil,bengali,gujarati,gurmukhi,odia,telugu,kannada,malayalam};
// Split-vowel bits correspond to block offsets 0x40..0x4f. They are stored in
// the descriptor's padding; a neighboring script can never match the mask.
struct SfntIndicScript {u32 tag;u16 block_first,split_vowels;};
static_assert(sizeof(SfntIndicScript)==8);
inline constexpr SfntIndicScript sfnt_indic_scripts[]{
    {sfnt_tag('d','e','v','2'),0x900,0},{sfnt_tag('t','m','l','2'),0xb80,0x1c00},{sfnt_tag('b','n','g','2'),0x980,0x1800},
    {sfnt_tag('g','j','r','2'),0xa80,0},{sfnt_tag('g','u','r','2'),0xa00,0},{sfnt_tag('o','r','y','2'),0xb00,0x1900},
    {sfnt_tag('t','e','l','2'),0xc00,0x100},{sfnt_tag('k','n','d','2'),0xc80,0xd81},{sfnt_tag('m','l','m','2'),0xd00,0x1c00}};
constexpr const SfntIndicScript* sfnt_indic_script(SfntIndicProfile profile) noexcept {
    const u32 index=static_cast<u8>(profile);return index<sizeof(sfnt_indic_scripts)/sizeof(sfnt_indic_scripts[0])?sfnt_indic_scripts+index:nullptr;
}
// Stage-local reuse: the nukta eligibility bit is unused before presentation. A Bengali
// frontend must repopulate this bit for word-initial forms before that stage.
inline constexpr u8 sfnt_bengali_initial=sfnt_deva_nukta;
using SfntIndicStage=SfntDevanagariStage;
using SfntIndicForm=SfntDevanagariForm;
// Modern script tags only, ENG then DefaultLangSys. Legacy script models and DFLT are
// not substituted for a missing modern script table. Both typography modes keep the
// required Indic features, calt/rclt and kern; no ccmp/liga/clig/rlig/rvrn or
// discretionary features are added.
// GSUB reuses lookups across stages, deduplicates within a stage and reserves an
// empty final-reorder stage. Required features join every executable stage with
// that stage's eligibility. GPOS is one unmasked stage.
// Scratch: lookup_count u16 cells. Output: four bytes per scheduled lookup.
// Views are borrowed and disjoint. Work is shared across selection, counting and
// emission. Short output reports capacity; any failure leaves output untouched.
SfntStatus sfnt_indic_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,SfntIndicProfile,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000,bool ordered_presentation=false) noexcept;
// Malayalam's no-post-form fallback schedules presentation features in feature
// order, merging calt/rclt last and keeping font lookup order in each group.
// A shared contextual lookup runs once per feature; repeated IDs
// inside a feature collapse. Select this only after resolving prepared font
// forms; other profiles reject it.
// Context-free font classification (form overload): locl plus exactly one of
// reph/half/pre/below/post, merged in font lookup order into one unmasked stage.
// Absent form features produce no roots (locl alone cannot establish form
// availability). Required features unrelated to these tags are not classification
// probes. The same storage, selection, variation and all-or-nothing output rules apply.
SfntStatus sfnt_indic_form_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,SfntIndicProfile,SfntIndicForm,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
}
