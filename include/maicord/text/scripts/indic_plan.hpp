#pragma once
#include "maicord/text/scripts/devanagari_plan.hpp"
namespace maicord {
// The nine modern Indic scripts of HarfBuzz's Indic shaper (Microsoft Indic shaping specs).
enum class SfntIndicProfile : u8 {devanagari,tamil,bengali,gujarati,gurmukhi,odia,telugu,kannada,malayalam};
struct SfntIndicScript {u32 tag;u32 block_first;};
static_assert(sizeof(SfntIndicScript)==8);
inline constexpr SfntIndicScript sfnt_indic_scripts[]{
    {sfnt_tag('d','e','v','2'),0x900},{sfnt_tag('t','m','l','2'),0xb80},{sfnt_tag('b','n','g','2'),0x980},
    {sfnt_tag('g','j','r','2'),0xa80},{sfnt_tag('g','u','r','2'),0xa00},{sfnt_tag('o','r','y','2'),0xb00},
    {sfnt_tag('t','e','l','2'),0xc00},{sfnt_tag('k','n','d','2'),0xc80},{sfnt_tag('m','l','m','2'),0xd00}};
constexpr const SfntIndicScript* sfnt_indic_script(SfntIndicProfile profile) noexcept {
    const u32 index=static_cast<u8>(profile);return index<sizeof(sfnt_indic_scripts)/sizeof(sfnt_indic_scripts[0])?sfnt_indic_scripts+index:nullptr;
}
using SfntIndicForm=SfntDevanagariForm;
// Modern script tags only, ENG then DefaultLangSys; fonts with only the older tags are not shaped as
// Indic. GSUB features and stages are HarfBuzz's for the Indic shaper: locl, ccmp (and rvrn) first, the
// basic features one stage each with their masks, an empty final-reordering stage, then init, pres,
// abvs, blws, psts, haln, rlig, calt, clig and rclt together (liga is off). A lookup shared by features
// of one stage runs once, unmasked if any of them is. The language system's required feature joins the
// stage of its tag (stage 0 for other tags), unmasked. GPOS is one stage: kern, dist, abvm, blwm, mark,
// mkmk and curs.
// Scratch: lookup_count u16 cells. Output: four bytes per scheduled lookup. The optional per_syllable
// bitset (at least (lookups+63)/64 words, written with the output) marks the steps HarfBuzz applies
// within each syllable: all but rvrn, rlig, calt, clig, rclt and a required feature of another tag.
// Views are borrowed and disjoint. Work is shared across selection, counting and
// emission. Short output reports capacity; any failure leaves output untouched.
SfntStatus sfnt_indic_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,SfntIndicProfile,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000,Slice<u64> per_syllable={}) noexcept;
// The lookups of one form feature for context-free font probes (HarfBuzz would_substitute): rphf, pref,
// blwf, pstf or vatu alone, in font lookup order, as one unmasked stage, with the required feature when
// its tag is that feature. An absent feature schedules nothing. The same storage, selection, variation
// and all-or-nothing output rules apply.
SfntStatus sfnt_indic_form_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,SfntIndicProfile,SfntIndicForm,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
}
