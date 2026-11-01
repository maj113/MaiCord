#pragma once
#include "maicord/text/context.hpp"
namespace maicord {
using SfntLookupVisitor=SfntStatus(*)(void*,u16) noexcept;
// Structural/action traversal only; selected payloads still require context_open.
SfntStatus sfnt_context_visit_lookups(ConstSlice<u8>,SfntContextKind,u16 lookup_limit,
    SfntLookupVisitor,void*,u32 work,u32* used) noexcept;
// Matches the context at start of a validated run: start<input_end<=glyph count,
// positions usable, output zeroed. Input is logical order; backtrack/lookahead
// may extend outside [start,input_end) within the run. The first input glyph is
// not filtered; backtrack order is nearest-first. Actions keep their encoded order
// and refer to matched input sequence positions, not raw run offsets, so the
// nested executor must rebase after prior edits. A rule with no actions still
// matches; no match returns ok with matched=false. On output_too_small only
// input_count is set, to the required scratch capacity. mask restricts the full
// input span (including ignored glyphs) as in GSUB basic matching; backtrack and
// lookahead need no eligibility bits. A non-sentinel first_coverage_index is the
// already queried first-input coverage index for this context and glyph, so a
// caller that has it avoids repeating the lookup. Charges *work.
SfntStatus context_match(const SfntContext&,const SfntGdef&,const SfntLookupFilter&,const SfntGlyphView&,u32 start,u32 input_end,
    Slice<u32> positions,SfntContextMatch*,u32* work,u8 mask,u32 first_coverage_index) noexcept;
}
