#pragma once
#include "maicord/text/gpos_program.hpp"
namespace maicord {
// Prepared single/pair subtable match at start<end of a validated view. Charges
// *work; output is written only on a match.
SfntStatus gpos_subtable_match(const SfntGposSubtable&,const SfntGdef&,const SfntLookupFilter&,const SfntGlyphView&,
    u32 start,u32 end,SfntGposMatch*,u32* work) noexcept;
// Internal continuation within one shaping operation, after a successful public
// program lookup validated every run field. The program, glyphs, metrics, flags
// and associations must remain unchanged. Only successful GPOS executors may
// have written positions; those writes preserve finite values. The coordinator
// has already validated the root index and checked its coverage bounds.
SfntStatus sfnt_gpos_execute_validated_lookup(const SfntGposProgram&,u16,SfntGlyphView,
    ConstSlice<double>,ConstSlice<u8>,ConstSlice<SfntMarkAssociation>,ConstSlice<u32>,Slice<SfntPosition>,
    u32,u32,bool,const SfntPositionInstance&,SfntNullAnchorPolicy,SfntComponentAnchorPolicy,
    SfntGposExecutionScratch,SfntGposExecutionLimits,SfntGposExecutionResult*) noexcept;
}
