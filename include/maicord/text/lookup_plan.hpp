#pragma once
#include "maicord/text/layout_directory.hpp"
namespace maicord {
struct SfntLookupPlanResult {u64 operations;u32 lookup_count,script_tag,language_tag;bool language_found;};
static_assert(sizeof(SfntLookupPlanResult)==24);
// Shared feature planning for the native alphabetic profiles. Common (tag zero) uses
// its separate frontend; Arabic and Indic profiles are not handled here.
bool sfnt_alphabetic_script_supported(u32 script_tag) noexcept;
// Latin/horizontal/en-US full-shaping profile. The caller selects the script and
// must not apply this profile to joining/reordering scripts. Selects latn, then
// DFLT if latn is absent; ENG, then that script's DefaultLangSys.
// Required features cannot be suppressed. Default GSUB: ccmp/locl/rlig/rclt/
// rvrn/liga/clig/calt. Default GPOS: dist/mark/mkmk/kern. Independent letters use
// ccmp/locl/rlig/rclt/rvrn/dist/mark/mkmk in both tables (explicit typography
// replaces optional defaults, including calt). All enabled parameters are one.
// Plans full shaping only; nominal-mapping eligibility, which can bypass
// shaping, is separate (nominal_participation.hpp, nominal_sources.hpp).
// Duplicate feature tags use the first LangSys-listed index. Lookup IDs are
// deduplicated and emitted in ascending order. Variations must come from this
// layout. Scratch: ceil(lookup_count/64) u64 words, disposable. Output: one u16
// per selected lookup, valid on success. Storage is borrowed and disjoint. With
// adequate scratch, short output reports the required lookup_count without
// writing output; short scratch returns output_too_small before lookups are
// counted. Work bounds directory, feature and bitset walks.
SfntStatus sfnt_latin_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent_letters,
    Slice<u64> scratch,Slice<u16> lookup_indices,SfntLookupPlanResult* result,u64 work_limit=1000000) noexcept;
// Already itemized Common/en-US, as used by emoji runs. DFLT, then latn;
// ENG, then DefaultLangSys. Uses the same default feature set, required-feature
// rules, deduplication, ordering, variation and storage contract as above.
SfntStatus sfnt_common_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,
    Slice<u64> scratch,Slice<u16> lookup_indices,SfntLookupPlanResult* result,u64 work_limit=1000000,bool independent_letters=false) noexcept;
// Scripts accepted by sfnt_alphabetic_script_supported share the feature policy
// above: the requested script, then latn, then DFLT; ENG then DefaultLangSys.
// Ordinary Hangul does not fall through when hang is absent; independent
// typography enables the fallback. The internal jamo profile selects hang GSUB
// features in ccmp/ljmo/vjmo/tjmo order, deduplicating within each stage but
// keeping repeated lookup IDs across stages; required features run in the first
// stage. Output capacity counts every emitted entry.
SfntStatus sfnt_alphabetic_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,u32 script_tag,bool independent,
    Slice<u64> scratch,Slice<u16> lookup_indices,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
}
