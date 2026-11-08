#pragma once
#include "maicord/text/scripts/ascii_run.hpp"
namespace maicord {
// A complete, already itemized Latin/en-US run in one selected native font.
// Text with marks is normalized as HarfBuzz does (decomposition and composition
// where the font maps the result, marks sorted by modified combining class);
// UTF-16 source positions are kept. Missing glyphs remain zero for caller-owned fallback.
// Storage has the aliasing/lifetime rules of native_ascii_run. Scalars borrow
// the components prefix; initial boundaries borrow targets; control provenance
// borrows advances until the post-GSUB policy has consumed it. No allocation.
// Failed attempts leave output lengths unset.
// With require_source_coverage, a scalar the font cannot map even after
// normalization returns unsupported_sequence so the caller can fall back.
// force_shaping carries the containing font/effect range's eligibility decision
// across script cuts. It keeps the same default features, not independent mode.
// paragraph_start: the text begins its paragraph (HarfBuzz HB_BUFFER_FLAG_BOT): a
// leading mark then follows a dotted circle when the font maps U+25CC.
SfntStatus native_latin_run(const NativeRunPlanStore&,NativeRunPlanHandle,Utf8View,
    bool independent_letters,float em_size,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool require_source_coverage=false,bool force_shaping=false,bool paragraph_start=true) noexcept;
// Generic fallback for a resolved script whose font selects its Latin program.
// Always shapes and preserves rich glyph/component output instead of compacting.
// Mapped input borrows positions; all source and lifetime rules match native_latin_run.
SfntStatus native_latin_shape_run(const NativeRunPlanStore&,NativeRunPlanHandle,Utf8View,bool independent,bool rtl,float em,
    const SfntShapeStorage&,SfntShapeRunResult*,SfntShapeLimits limits={}) noexcept;
}
