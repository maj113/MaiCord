#pragma once
#include "maicord/text/opentype/gsub_execute.hpp"
namespace maicord {
// Prepared type 1-4 subtable match at start of a validated view with position
// scratch. Input is logical glyph order, limited to the applicable feature range;
// the caller maintains glyph classes, including when GDEF omits classification.
// The first glyph is tested regardless of filter flags. Glyphs skipped between
// ligature components are not consumed, and positions lists only consumed
// glyphs. The first matching ligature wins; alternate is zero-based. No match
// returns ok with matched=false. On output_too_small, input_count holds the
// required position capacity and other result fields are empty. Position scratch
// is valid only after a successful match and must not overlap the input, font
// mapping or prepared storage. Work charged per candidate is unaffected by reuse
// of a filtered input prefix. A nonzero mask requires intersecting eligibility
// bits in the glyphs' reserved byte across the whole input span, including
// glyphs skipped by lookup filtering; zero disables the check. Charges *work.
SfntStatus gsub_subtable_match(const SfntGsubSubtable&,const SfntGdef&,const SfntLookupFilter&,const SfntGlyphView&,
    u32 start,u32 alternate,Slice<u32> positions,SfntGsubMatch*,u32* work,u8 mask) noexcept;
// Internal continuation that skips the argument and run validation of
// sfnt_gsub_execute_lookup. Valid only after a successful public lookup on this
// buffer, with the program and scratch views unchanged, the root lookup prepared
// and start/end range-checked. Substitutions keep records, component references
// and source indices valid, but the caller must refresh glyph bounds; a script
// callback that changes the run requires another public lookup first. Every
// lookup action is still executed and budgeted. Empty segments run the plain lookup; otherwise segments must
// already be valid for this run (see sfnt_gsub_execute_lookup).
SfntStatus sfnt_gsub_execute_validated_lookup(const SfntGsubProgram&,const SfntGsubExecutableLookup&,
    SfntGlyphBuffer*,u32,u32,u32,SfntGsubExecutionScratch,SfntGsubExecutionLimits,
    SfntGsubExecutionResult*,SfntClusterMap*,SfntComponentForest*,u8,ConstSlice<u32> segments) noexcept;
}
