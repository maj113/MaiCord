#pragma once
#include "maicord/text/scripts/devanagari_plan.hpp"
#include "maicord/text/opentype/gsub_execute.hpp"
namespace maicord {
// HarfBuzz hb_ot_layout_lookup_would_substitute over the scheduled lookups of one feature: whether any
// of them has a rule whose input is exactly these glyphs. Single, multiple and alternate rules take one
// glyph, ligatures and (chained) context rules the whole sequence; lookup flags are not applied. With
// zero_context a chained rule must also have no backtrack or lookahead. Steps are prepared lookups of
// the program; *work is charged per rule examined. Deferred lookups are prepared on demand.
SfntStatus sfnt_gsub_would_substitute(const SfntGsubProgram&,ConstSlice<SfntLookupStep>,ConstSlice<u16> glyphs,
    bool zero_context,bool* result,u64* work) noexcept;
}
