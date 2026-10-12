#pragma once
#include "maicord/text/gsub_execute.hpp"
namespace maicord {
// Internal continuation after a successful public lookup in this same shape.
// Successful substitutions preserve valid records, component references and
// source indices; glyph bounds must be refreshed separately after edits.
// Program and scratch views remain unchanged. Script callbacks invalidate this
// permission until another public lookup validates the changed run.
// The root lookup is prepared and its current range has been checked.
// This entry point still executes and budgets every lookup action.
SfntStatus sfnt_gsub_execute_validated_lookup(const SfntGsubProgram&,const SfntGsubExecutableLookup&,
    SfntGlyphBuffer*,u32,u32,u32,SfntGsubExecutionScratch,SfntGsubExecutionLimits,
    SfntGsubExecutionResult*,SfntClusterMap*,SfntComponentForest*,u8) noexcept;
}
