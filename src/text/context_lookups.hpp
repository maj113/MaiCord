#pragma once
#include "maicord/text/context.hpp"
namespace maicord {
using SfntLookupVisitor=SfntStatus(*)(void*,u16) noexcept;
// Structural/action traversal only; selected payloads still require context_open.
SfntStatus sfnt_context_visit_lookups(ConstSlice<u8>,SfntContextKind,u16 lookup_limit,
    SfntLookupVisitor,void*,u32 work,u32* used) noexcept;
// sfnt_context_match_view after its argument checks, for executors whose run
// is validated: start<input_end<=glyph count, positions usable, output zeroed.
// Charges *work.
SfntStatus context_match(const SfntContext&,const SfntGdef&,const SfntLookupFilter&,const SfntGlyphView&,u32 start,u32 input_end,
    Slice<u32> positions,SfntContextMatch*,u32* work,u8 mask,u32 first_coverage_index) noexcept;
}
