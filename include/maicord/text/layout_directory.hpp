#pragma once
#include "maicord/text/sfnt.hpp"

namespace maicord {
enum class SfntLayoutKind : u8 { substitution,positioning };
inline constexpr u32 sfnt_default_language=0xffffffffu;
inline constexpr u32 sfnt_no_variation_record=0xffffffffu;
struct SfntLayout {ConstSlice<u8> table,scripts,features,lookups;u32 variations;u16 script_count,feature_count,lookup_count;SfntLayoutKind kind;};
struct SfntScript {ConstSlice<u8> table;u16 language_count;};
struct SfntLanguage {ConstSlice<u8> feature_indices;u16 required_feature;};
struct SfntLanguageSelection {SfntLanguage language;u32 script_tag,language_tag;};
static_assert(sizeof(SfntLanguageSelection)==32);
struct SfntFeature {ConstSlice<u8> table,lookup_indices;u16 params_offset;};
struct SfntLookup {ConstSlice<u8> table;u16 type,source_type,flags,subtable_count,mark_set;};
struct SfntFeatureSubstitutions {ConstSlice<u8> table;u16 count;};
static_assert(sizeof(SfntLayout)==80 && sizeof(SfntScript)==24 && sizeof(SfntFeature)==40 && sizeof(SfntLookup)==32);
// Opens prepare directories, not the entire lookup graph. All views borrow the
// enclosing immutable mapping; queries require successfully prepared views.
// Feature indices retain their original identity, including duplicate tags and
// arbitrary LangSys index order. Script/language selection and fallbacks are
// caller policy. Feature parameters and lookup payloads are not decoded here.
SfntStatus sfnt_layout_open(ConstSlice<u8> bytes,SfntLayoutKind kind,SfntLayout* output) noexcept;
SfntStatus sfnt_layout_find_script(const SfntLayout& layout,u32 tag,u16* index) noexcept;
SfntStatus sfnt_layout_script(const SfntLayout& layout,u16 index,u32* tag,SfntScript* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus sfnt_script_find_language(const SfntScript& script,u32 tag,u16* index) noexcept;
SfntStatus sfnt_script_language(const SfntScript& script,u16 feature_limit,u32 index,u32* tag,SfntLanguage* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Resolve caller-ordered script candidates, then the requested language or that
// script's DefaultLangSys. A zero requested language means DefaultLangSys only.
// Stop at the first present script even if it has no usable language. Returns
// missing_table for absent script/language; script_tag distinguishes those cases.
// This is a shared directory walk, not Unicode-to-script or shaping policy.
// A successful result borrows the font's feature-index slice without allocation.
// Storage is disjoint; work accounts for each search and validated directory.
SfntStatus sfnt_layout_select_language(const SfntLayout&,ConstSlice<u32> script_candidates,u32 requested_language,
    SfntLanguageSelection*,u64 work_limit=1000000,u64* work_used=nullptr) noexcept;
// Pass an empty substitutions view for the base feature. Otherwise use a view
// selected for this same layout. Required-feature activation and stage ordering
// belong to the shaper; these functions preserve indices rather than sort them.
SfntStatus sfnt_layout_feature(const SfntLayout& layout,u16 index,const SfntFeatureSubstitutions& substitutions,u32* tag,SfntFeature* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus sfnt_layout_lookup(const SfntLayout& layout,u16 index,SfntLookup* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Lookup type is the resolved type; source_type preserves the extension wrapper.
// Preparing a lookup checks every wrapper has the same non-extension type.
SfntStatus sfnt_lookup_subtable(const SfntLookup& lookup,u16 index,ConstSlice<u8>* output) noexcept;
// Invoke for variable fonts with all post-avar F2DOT14 coordinates in fvar order.
// First matching supported record wins, including a null substitution. Unknown
// condition formats, invalid axis indices and unsupported substitution versions
// skip that record as specified. Malformed spans and exhausted work are errors.
// Empty output with record_index == sfnt_no_variation_record means no match.
SfntStatus sfnt_layout_variation_select(const SfntLayout& layout,ConstSlice<i16> coordinates,
                                      SfntFeatureSubstitutions* output,u32* record_index,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
}
