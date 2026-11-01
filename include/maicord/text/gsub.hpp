#pragma once
#include "maicord/text/gdef.hpp"
#include "maicord/text/layout_directory.hpp"
#include "maicord/text/glyph_buffer.hpp"

namespace maicord {
struct SfntGsubSubtable {ConstSlice<u8> table;SfntCoverage coverage;u32 glyph_limit;u16 type,format;};
struct SfntGsubMatch {ConstSlice<u8> replacements;u32 input_count,last_input;u16 single,output_count;bool matched;};
static_assert(sizeof(SfntGsubSubtable)==40 && sizeof(SfntGsubGlyph)==4 && sizeof(SfntGsubMatch)==32);
// Types 1..4 only. Opens validate coverage, referenced glyph IDs and child
// arrays within an aggregate work limit. Font bytes and prepared views must stay
// alive and unchanged. Optional work_used reports charged units on every
// return from individual opens/matches, so an enclosing executor can share one
// budget. Validation is strict by default; ignore_empty_sequences skips empty
// MultipleSubst rules without deleting input or blocking later subtables.
SfntStatus sfnt_gsub_subtable_open(ConstSlice<u8> bytes,u16 type,u32 glyph_limit,SfntGsubSubtable* output,u32 work_limit=1000000,u32* work_used=nullptr,bool ignore_empty_sequences=false) noexcept;
}
