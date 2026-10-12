#pragma once
#include "maicord/text/layout_directory.hpp"
namespace maicord {
struct SfntLookupPlanResult {u64 operations;u32 lookup_count,script_tag,language_tag;bool language_found;};
static_assert(sizeof(SfntLookupPlanResult)==24);
// Shared feature planning for the native alphabetic profiles. Common (tag zero) uses
// its separate frontend; Arabic and Indic profiles are not handled here.
bool sfnt_alphabetic_script_supported(u32 script_tag) noexcept;
// MaiCord's Latin/horizontal/en-US full-shaping profile. The caller selects
// the script; do not apply this profile to joining/reordering scripts. Selects
// latn, then DFLT if latn is absent; ENG, then that script's DefaultLangSys.
// Required features cannot be suppressed. Default GSUB: ccmp/locl/rlig/rclt/
// rvrn/liga/clig/calt. Default GPOS: dist/mark/mkmk/kern. Independent letters use
// ccmp/locl/rlig/rclt/rvrn/dist/mark/mkmk in both tables (explicit typography
// replaces optional defaults, including calt). All enabled parameters are one.
// This plans full shaping only. TextLayout's nominal-mapping eligibility can
// bypass shaping and has separate compatibility requirements. A table-participation
// pass exists; full cmap/Unicode/range eligibility is not implemented here.
// Duplicate feature tags use the first LangSys-listed index. Lookup IDs are
// deduplicated and emitted in ascending order. Variations belong to this layout.
// Scratch: ceil(lookup_count/64) u64 words. Output: one u16 per selected lookup.
// All storage is borrowed/disjoint. No allocation. Output is valid on success.
// With adequate scratch, short output reports the required lookup_count without
// writing output. Short scratch returns output_too_small before counting lookups.
// Scratch is disposable. Shared work bounds directory, feature and bitset walks.
SfntStatus sfnt_latin_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent_letters,
    Slice<u64> scratch,Slice<u16> lookup_indices,SfntLookupPlanResult* result,u64 work_limit=1000000) noexcept;
// Already itemized Common/en-US, as used by emoji runs. DFLT, then latn;
// ENG, then DefaultLangSys. Uses the same default feature set, required-feature
// rules, deduplication, ordering, variation and storage contract as above.
SfntStatus sfnt_common_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,
    Slice<u64> scratch,Slice<u16> lookup_indices,SfntLookupPlanResult* result,u64 work_limit=1000000,bool independent_letters=false) noexcept;
// Greek/Cyrillic/Hebrew/Han/kana/Armenian/Georgian profiles share the
// feature policy above. Armenian and Georgian use armn and geor respectively.
// Select the requested script, then latn, then DFLT; ENG then DefaultLangSys.
// Also accepts hang and the internal jamo profile. Ordinary Hangul does not
// fall through when hang is absent; independent typography enables fallback.
// jamo selects hang GSUB features in ccmp/ljmo/vjmo/tjmo order, deduplicating
// within each stage but retaining repeated lookup IDs across stages. Required
// features run in the first stage. Output capacity counts every emitted entry.
// Character preparation is separate. CJK width/vertical/alternate features are not enabled by default.
SfntStatus sfnt_alphabetic_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,u32 script_tag,bool independent,
    Slice<u64> scratch,Slice<u16> lookup_indices,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
}
