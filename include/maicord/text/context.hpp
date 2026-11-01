#pragma once
#include "maicord/text/gsub.hpp"

namespace maicord {
enum class SfntContextKind : u8 { sequence,chained };
struct SfntContextRequirements {u32 coverages,classes;};
struct SfntContext {ConstSlice<u8> table;ConstSlice<SfntCoverage> coverages;ConstSlice<SfntClassDef> classes;u32 glyph_limit;u16 format,max_inputs;SfntContextKind kind;};
struct SfntContextMatch {ConstSlice<u8> actions;u32 input_count,last_input;bool matched;};
struct SfntReverseSubstitution {ConstSlice<u8> table;ConstSlice<SfntCoverage> coverages;u32 glyph_limit;u16 backtrack_count,lookahead_count;};
static_assert(sizeof(SfntContext)==64 && sizeof(SfntContextMatch)==32 && sizeof(SfntReverseSubstitution)==40);
// Requirements inspect structural headers only, not child data. Opens validate
// referenced data into caller-owned storage, valid only on success. Views and font
// mappings must stay alive, immutable and disjoint from match scratch. Optional
// work_used reports charged units on every open/match return so nested execution can
// keep an aggregate budget; it must not alias other inputs.
SfntStatus sfnt_context_requirements(ConstSlice<u8> bytes,SfntContextKind kind,SfntContextRequirements* output) noexcept;
SfntStatus sfnt_context_open(ConstSlice<u8> bytes,SfntContextKind kind,u32 glyph_limit,u16 lookup_limit,
                            Slice<SfntCoverage> coverages,Slice<SfntClassDef> classes,SfntContext* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// GSUB type 8, format 1. The executor visits start positions in reverse order;
// this function matches one position and excludes contexts from consumed input.
SfntStatus sfnt_gsub_reverse_requirements(ConstSlice<u8> bytes,u32* coverages) noexcept;
SfntStatus sfnt_gsub_reverse_open(ConstSlice<u8> bytes,u32 glyph_limit,Slice<SfntCoverage> coverages,
                                SfntReverseSubstitution* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus sfnt_gsub_reverse_match_view(const SfntReverseSubstitution& reverse,const SfntGdef& gdef,const SfntLookupFilter& filter,
                                 SfntGlyphView glyphs,u32 start,SfntGsubMatch* output,u32 work_limit=1000000,u32* work_used=nullptr,u8 feature_mask=0) noexcept;
}
