#pragma once
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// A complete, already itemized Latin/en-US run in one selected native font.
// UTF-16 source positions are kept through font-aware composition; user
// text is not normalized. Missing glyphs remain zero for caller-owned fallback.
// Script selection, bidi and fallback are not performed by this entry point.
// Storage has the aliasing/lifetime rules of native_ascii_run. Scalars borrow
// the components prefix; initial boundaries borrow targets; control provenance
// borrows advances until the post-GSUB policy has consumed it. No allocation,
// OS calls or external library. Failed attempts leave output lengths unset.
// Layout callers can require coverage of every original scalar before font-aware
// composition; a missing original then returns unsupported_sequence for fallback.
// force_shaping carries the containing font/effect range's eligibility decision
// across script cuts. It keeps the same default features, not independent mode.
SfntStatus native_latin_run(const NativeRunPlanStore&,NativeRunPlanHandle,Utf8View,
    bool independent_letters,float em_size,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},bool require_source_coverage=false,bool force_shaping=false) noexcept;
// Generic fallback for a resolved script whose font selects its Latin program.
// Always shapes and preserves rich glyph/component output instead of compacting.
// Mapped input borrows positions; all source and lifetime rules match native_latin_run.
SfntStatus native_latin_shape_run(const NativeRunPlanStore&,NativeRunPlanHandle,Utf8View,bool independent,bool rtl,float em,
    SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={}) noexcept;
// Generic preparation for Khmer text that the font gate rejects. Uses
// caller-selected roots over the parent run, nominal dependent vowels and
// separate Khmer consonant shaping groups; editor graphemes are unaffected.
SfntStatus native_khmer_fallback_shape_run(const NativeRunPlanStore&,NativeRunPlanHandle,Utf8View,bool independent,bool rtl,float em,
    SfntShapeStorage,SfntShapeRunResult*,ConstSlice<u16> substitutions,ConstSlice<u16> positioning,SfntShapeLimits limits={}) noexcept;
}
