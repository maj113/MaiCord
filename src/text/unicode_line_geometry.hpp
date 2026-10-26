#pragma once
#include "maicord/text/ascii_line.hpp"
namespace maicord {
// Internal post-shaping geometry shared by resolved Latin and Common/emoji.
// The caller initializes line with only its successful run populated and keeps
// it private until this stage succeeds. Scratch/output can change on failure.
// Paragraph callers need only cluster widths and ordered-x validation: source
// boundaries were validated by the shaper and the collector owns line geometry.
SfntStatus native_unicode_line_geometry(const SfntFontLayoutMetrics&,const Utf8View&,float,
    const NativeAsciiLineStorage&,NativeAsciiLineResult*,bool paragraph_only=false) noexcept;
}
