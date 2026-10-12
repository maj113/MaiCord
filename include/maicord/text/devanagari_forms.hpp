#pragma once
#include "maicord/text/devanagari_plan.hpp"
#include "maicord/text/gsub_execute.hpp"
namespace maicord {
struct SfntDevanagariFormResult {u64 operations;u16 glyph;bool formed;};
// Test a mapped consonant/halant pair in a prepared GSUB program.
// Reph/half use C,H; pre/below/post use H,C. Steps come from the matching
// form_lookup_plan, in one unmasked stage. No surrounding glyph context exists.
// A positive result requires a single final glyph that covers both original source
// components: deleting one input or merely changing a glyph is not a form.
// Initial and between-root classes come from GDEF. No GPOS/metrics/shaping
// export, allocation, cache or platform text service is involved.
// Scratch and glyph storage are disjoint from each other/steps/program/result.
// glyphs needs at least two slots; fonts may require more transient expansion.
// Result is written only on success; operations is for diagnostics on error.
// A buffer limit never turns an incomplete query into a negative answer.
SfntStatus sfnt_devanagari_probe_form(const SfntGsubProgram&,ConstSlice<SfntLookupStep>,
    u16 consonant,u16 halant,SfntDevanagariForm,Slice<SfntComponentGlyph> glyphs,
    SfntGsubExecutionScratch,SfntDevanagariFormResult*,SfntGsubExecutionLimits limits={1000000,100000}) noexcept;
}
