#pragma once
#include "maicord/text/gsub.hpp"

namespace maicord {
enum class SfntContextKind : u8 { sequence,chained };
struct SfntContextRequirements {u32 coverages,classes;};
struct SfntContext {ConstSlice<u8> table;ConstSlice<SfntCoverage> coverages;ConstSlice<SfntClassDef> classes;u32 glyph_limit;u16 lookup_limit,format,max_inputs;SfntContextKind kind;};
struct SfntContextMatch {ConstSlice<u8> actions;u32 input_count,last_input;bool matched;};
struct SfntSequenceAction {u16 sequence_index,lookup_index;};
struct SfntReverseSubstitution {ConstSlice<u8> table;ConstSlice<SfntCoverage> coverages;u32 glyph_limit;u16 backtrack_count,lookahead_count;};
static_assert(sizeof(SfntContext)==64 && sizeof(SfntContextMatch)==32 && sizeof(SfntReverseSubstitution)==40);
// Requirements inspect structural headers only, not all child data. Opens
// validate referenced data into caller-owned storage, which is valid only on
// success. Views and font mappings must remain alive, immutable and disjoint
// from match scratch. No allocation or recursive lookup execution occurs here.
// Optional work_used reports charged units on all open/match returns so nested
// execution can retain an aggregate budget. It must not alias other inputs.
SfntStatus sfnt_context_requirements(ConstSlice<u8> bytes,SfntContextKind kind,SfntContextRequirements* output) noexcept;
SfntStatus sfnt_context_open(ConstSlice<u8> bytes,SfntContextKind kind,u32 glyph_limit,u16 lookup_limit,
                            Slice<SfntCoverage> coverages,Slice<SfntClassDef> classes,SfntContext* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Input is logical order. start<input_end<=glyphs.count bounds input matching;
// backtrack/lookahead may extend outside that feature range within the run.
// The first input glyph is not filtered. Backtrack order is nearest-first.
// Actions keep their encoded order and refer to matched input sequence positions,
// not raw run offsets. The nested executor must rebase after prior edits.
// A zero-action matching rule still matches. No match returns ok/matched=false.
// On output_too_small only input_count is set to the required scratch capacity.
// feature_mask restricts the full input span (including ignored glyphs) as in
// GSUB basic matching. Backtrack and lookahead do not require eligibility bits.
// A non-sentinel first_coverage_index must be the already queried first-input
// coverage index for this context and current glyph. Executors reuse
// that result after rejecting impossible subtables; ordinary callers omit it.
SfntStatus sfnt_context_match_view(const SfntContext& context,const SfntGdef& gdef,const SfntLookupFilter& filter,
                             SfntGlyphView glyphs,u32 start,u32 input_end,Slice<u32> positions,
                             SfntContextMatch* output,u32 work_limit=1000000,u32* work_used=nullptr,u8 feature_mask=0,u32 first_coverage_index=sfnt_not_covered) noexcept;
inline SfntStatus sfnt_context_match(const SfntContext& context,const SfntGdef& gdef,const SfntLookupFilter& filter,
                                    ConstSlice<SfntGsubGlyph> glyphs,u32 start,u32 input_end,Slice<u32> positions,SfntContextMatch* output,u32 work_limit=1000000) noexcept {
    return sfnt_context_match_view(context,gdef,filter,sfnt_glyph_view(glyphs),start,input_end,positions,output,work_limit);
}
SfntStatus sfnt_context_action(const SfntContextMatch& match,u32 index,SfntSequenceAction* output) noexcept;
// GSUB type 8, format 1. The executor visits start positions in reverse order;
// this function matches one position and excludes contexts from consumed input.
SfntStatus sfnt_gsub_reverse_requirements(ConstSlice<u8> bytes,u32* coverages) noexcept;
SfntStatus sfnt_gsub_reverse_open(ConstSlice<u8> bytes,u32 glyph_limit,Slice<SfntCoverage> coverages,
                                SfntReverseSubstitution* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus sfnt_gsub_reverse_match_view(const SfntReverseSubstitution& reverse,const SfntGdef& gdef,const SfntLookupFilter& filter,
                                 SfntGlyphView glyphs,u32 start,SfntGsubMatch* output,u32 work_limit=1000000,u32* work_used=nullptr,u8 feature_mask=0) noexcept;
inline SfntStatus sfnt_gsub_reverse_match(const SfntReverseSubstitution& reverse,const SfntGdef& gdef,const SfntLookupFilter& filter,
                                        ConstSlice<SfntGsubGlyph> glyphs,u32 start,SfntGsubMatch* output,u32 work_limit=1000000) noexcept {
    return sfnt_gsub_reverse_match_view(reverse,gdef,filter,sfnt_glyph_view(glyphs),start,output,work_limit);
}
}
