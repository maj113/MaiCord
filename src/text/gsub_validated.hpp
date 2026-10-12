#pragma once
#include "maicord/text/gsub_execute.hpp"
namespace maicord {
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
