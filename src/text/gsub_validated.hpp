#pragma once
#include "maicord/text/gsub_execute.hpp"
namespace maicord {
// Prepared type 1-4 subtable match at start of a validated view with position
// scratch. Charges *work; see sfnt_gsub_subtable_match_view for the result.
SfntStatus gsub_subtable_match(const SfntGsubSubtable&,const SfntGdef&,const SfntLookupFilter&,SfntGlyphView,
    u32 start,u32 alternate,Slice<u32> positions,SfntGsubMatch*,u32* work,u8 mask) noexcept;
// Internal continuation that skips the argument and run validation of
// sfnt_gsub_execute_lookup. Valid only after a successful public lookup on this
// buffer, with the program and scratch views unchanged, the root lookup prepared
// and start/end range-checked. Substitutions keep records, component references
// and source indices valid, but the caller must refresh glyph bounds; a script
// callback that changes the run requires another public lookup first. Every
// lookup action is still executed and budgeted.
SfntStatus sfnt_gsub_execute_validated_lookup(const SfntGsubProgram&,const SfntGsubExecutableLookup&,
    SfntGlyphBuffer*,u32,u32,u32,SfntGsubExecutionScratch,SfntGsubExecutionLimits,
    SfntGsubExecutionResult*,SfntClusterMap*,SfntComponentForest*,u8) noexcept;
}
