#pragma once
#include "maicord/text/gdef.hpp"
#include "maicord/text/layout_directory.hpp"
#include "maicord/text/glyph_buffer.hpp"

namespace maicord {
struct SfntGsubSubtable {ConstSlice<u8> table;SfntCoverage coverage;u32 glyph_limit;u16 type,format,max_inputs;};
struct SfntGsubMatch {ConstSlice<u8> replacements;u32 input_count,last_input;u16 single,output_count;bool matched;};
struct SfntGsubLookup {ConstSlice<SfntGsubSubtable> subtables;SfntLookupFilter filter;u16 max_inputs;};
static_assert(sizeof(SfntGsubSubtable)==48 && sizeof(SfntGsubGlyph)==4 && sizeof(SfntGsubMatch)==32 && sizeof(SfntGsubLookup)==40);
// Types 1..4 only. Opens validate coverage, all referenced glyph IDs and child
// arrays against an aggregate work limit. Input bytes and prepared views must stay
// alive and unchanged. Views allocate no memory; lookup storage is caller-owned
// and valid only after successful preparation.
// Optional work_used reports charged units on every return from individual
// subtable opens/matches, so an enclosing executor can share one budget.
// The compatibility option ignores empty MultipleSubst rules without deleting
// input or preventing later subtables from matching. Strict validation is the default.
SfntStatus sfnt_gsub_subtable_open(ConstSlice<u8> bytes,u16 type,u32 glyph_limit,SfntGsubSubtable* output,u32 work_limit=1000000,u32* work_used=nullptr,bool ignore_empty_sequences=false) noexcept;
SfntStatus sfnt_gsub_lookup_prepare(const SfntLayout& layout,u16 index,const SfntGdef& gdef,u32 glyph_limit,
                                  Slice<SfntGsubSubtable> storage,SfntGsubLookup* output,u32 work_limit=1000000) noexcept;
// Input is logical glyph order, limited to the applicable feature range. Classes
// are maintained by the caller, including when GDEF omits glyph classification.
// The first glyph is tested regardless of filter flags. Skipped glyphs between
// ligature components are not consumed, and input positions identify only consumed
// glyphs. First matching ligature/subtable wins; alternate is a zero-based choice.
// No match returns ok with matched=false. On output_too_small, input_count holds
// the required position capacity; other result fields are empty. Scratch is valid
// only after a successful match. This API does not mutate clusters or the glyph run.
// Matching requires a successfully prepared subtable and unchanged font bytes/views.
// Reusing a filtered input prefix keeps the per-candidate work charge unchanged.
// Position scratch must not overlap the input, font mapping or prepared storage.
// A nonzero feature_mask requires intersecting eligibility bits in reserved for
// the complete input span, including glyphs skipped by lookup filtering. This
// matches Windows form-feature ranges. Zero disables this restriction.
SfntStatus sfnt_gsub_subtable_match_view(const SfntGsubSubtable& subtable,const SfntGdef& gdef,const SfntLookupFilter& filter,
                                  SfntGlyphView glyphs,u32 start,u32 alternate,Slice<u32> positions,
                                  SfntGsubMatch* output,u32 work_limit=1000000,u32* work_used=nullptr,u8 feature_mask=0) noexcept;
SfntStatus sfnt_gsub_lookup_match_view(const SfntGsubLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,
                                u32 start,u32 alternate,Slice<u32> positions,SfntGsubMatch* output,u32 work_limit=1000000,u8 feature_mask=0) noexcept;
inline SfntStatus sfnt_gsub_subtable_match(const SfntGsubSubtable& subtable,const SfntGdef& gdef,const SfntLookupFilter& filter,
                                         ConstSlice<SfntGsubGlyph> glyphs,u32 start,u32 alternate,Slice<u32> positions,SfntGsubMatch* output,u32 work_limit=1000000) noexcept {
    return sfnt_gsub_subtable_match_view(subtable,gdef,filter,sfnt_glyph_view(glyphs),start,alternate,positions,output,work_limit);
}
inline SfntStatus sfnt_gsub_lookup_match(const SfntGsubLookup& lookup,const SfntGdef& gdef,ConstSlice<SfntGsubGlyph> glyphs,
                                       u32 start,u32 alternate,Slice<u32> positions,SfntGsubMatch* output,u32 work_limit=1000000) noexcept {
    return sfnt_gsub_lookup_match_view(lookup,gdef,sfnt_glyph_view(glyphs),start,alternate,positions,output,work_limit);
}
SfntStatus sfnt_gsub_replacement(const SfntGsubMatch& match,u16 index,u16* glyph) noexcept;
}
