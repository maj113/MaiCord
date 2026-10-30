#pragma once
#include "maicord/text/ascii_paragraph.hpp"
namespace maicord {
// Internal post-shaping geometry shared by resolved Latin and Common/emoji.
// The caller initializes line with only its successful run populated and keeps
// it private until this stage succeeds. Scratch/output can change on failure.
// Paragraph callers need only cluster widths and ordered-x validation: source
// boundaries were validated by the shaper and the collector owns line geometry.
// printable_ascii text holds only U+0020..U+007E, so only the space is whitespace.
enum class NativeLineText:u8 {unicode,printable_ascii,paragraph};
SfntStatus native_unicode_line_geometry(const SfntFontLayoutMetrics&,const Utf8View&,float,
    const NativeAsciiLineStorage&,NativeAsciiLineResult*,NativeLineText) noexcept;
// Completes the shaped `line` (only its run populated): trailing spacing (none
// when zero), the font's line metrics and cluster geometry. *output is written
// only on success.
SfntStatus native_finish_line(const NativeRunPlanStore&,NativeRunPlanHandle,const Utf8View&,float em,const NativeAsciiLineStorage&,
    NativeAsciiLineResult& line,NativeAsciiLineResult* output,SfntShapeLimits limits,float trailing_spacing,bool rtl,NativeLineText) noexcept;
// Paragraph assembly shared by the ASCII, alphabetic and emoji frontends. A
// control item (tab, newline, nonvisual) of `units` source units becomes one
// glyphless cluster; a shaped line (in s.work) is appended with its glyphs,
// source map and clusters. Both advance `shaped`.
SfntStatus native_paragraph_append_control(const NativeAsciiParagraphStorage&,NativeParagraphShape&,u32 units,u32 flags) noexcept;
SfntStatus native_paragraph_append_line(const NativeAsciiParagraphStorage&,const NativeAsciiLineResult&,NativeParagraphShape&) noexcept;
}
