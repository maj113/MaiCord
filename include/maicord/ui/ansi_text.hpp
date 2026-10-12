#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/ui/text_style.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {

struct ShapedText;

// Scanner for ```ansi code blocks only (inline code and every other language
// keep the raw text), following the ansi-to-html tokenizer and tag stack:
// foreground defaults to the theme's text colour, background to transparent;
// colours 0-15 are the theme's ansi tokens, 16-255 a fixed xterm palette.
//   ESC[...m   SGR, each ';' code applied in order: 0 reset (closes every tag),
//              1 bold, 3 italic, 4 underline, 9 strikethrough, 8 hidden,
//              22 bold/italic off (CSS cannot remove an ancestor's underline),
//              23/24 close italic/underline only when that tag is innermost,
//              30-37/90-97 foreground, 40-47/100-107 background, 39 default
//              foreground, 49 transparent background (an outer background
//              still shows through), 5/6/53 push a tag without visible effect
//              here, anything else is ignored.
//   ESC[38;5;Nm / ESC[48;5;Nm  xterm colour N (canonical decimal, N <= 255).
//   ESC[38;2;R;G;Bm / ESC[48;2;R;G;Bm  truecolor, drawn with the nearest fixed
//              xterm colour (16-255) because spans carry palette indices.
//   ESC[K, ESC[(B, ESC[nJ, ESC[n;mf and an ESC with up to three digits or
//   ';' (optionally after '[') are removed; backspaces are removed.
// Everything else, including CR and LF, is text; hidden text is dropped.
// Colour codes are 0 (none: the code block's own colour or no background) or
// 1 + an xterm index. An underline or line-through takes the foreground of the
// run it sits under (the code block's own colour where the run has none),
// whatever colour was in effect when its 4/9 opened.
inline constexpr u16 kAnsiColorNone = 0;
inline constexpr u32 kAnsiTextStyleMask =
    text_style_bold | text_style_italic | text_style_underline | text_style_strikethrough;

struct AnsiTextStyle {
    u32 flags; // kAnsiTextStyleMask bits
    u16 foreground;
    u16 background;
};

constexpr bool operator==(const AnsiTextStyle& a, const AnsiTextStyle& b) noexcept {
    return a.flags == b.flags && a.foreground == b.foreground && a.background == b.background;
}

// Receives the visible text pieces in source order. Returning false stops.
using AnsiTextSink = bool (*)(void* context, u32 begin, u32 end, AnsiTextStyle style) noexcept;

// One linear pass over the input; false only when the sink stops it.
bool ansi_text_scan(Utf8View input, AnsiTextSink sink, void* context) noexcept;

// Code-block spans carry their colours in TextStyleSpan::reserved: bytes 0/1
// hold the foreground/background xterm index, byte 2 bits 0/1 say they are set.
constexpr void ansi_span_set_colors(TextStyleSpan& span, u16 foreground, u16 background) noexcept {
    span.reserved[0] = foreground ? static_cast<u8>(foreground - 1u) : u8{0};
    span.reserved[1] = background ? static_cast<u8>(background - 1u) : u8{0};
    span.reserved[2] =
        static_cast<u8>((span.reserved[2] & ~3u) | (foreground ? 1u : 0u) | (background ? 2u : 0u));
}
constexpr u16 ansi_span_foreground(const TextStyleSpan& span) noexcept {
    return (span.reserved[2] & 1u) ? static_cast<u16>(span.reserved[0] + 1u) : kAnsiColorNone;
}
constexpr u16 ansi_span_background(const TextStyleSpan& span) noexcept {
    return (span.reserved[2] & 2u) ? static_cast<u16>(span.reserved[1] + 1u) : kAnsiColorNone;
}

// Background rectangles are TextDecorationKind::background decorations with
// reserved1 set; reserved0 is the xterm index.
constexpr TextDecoration ansi_background_decoration(float left, float top, float right, float bottom,
                                                    u32 flags, u16 background) noexcept {
    return TextDecoration{left, top, right, bottom, flags, TextDecorationKind::background,
                          static_cast<u8>(background - 1u), 1, 0};
}
constexpr u16 ansi_decoration_background(const TextDecoration& decoration) noexcept {
    return decoration.kind == TextDecorationKind::background && decoration.reserved1 == 1
               ? static_cast<u16>(decoration.reserved0 + 1u)
               : kAnsiColorNone;
}
// Underline and line-through quads under a run with an ANSI foreground carry
// reserved1 = 2 and that xterm index in reserved0; others keep the text's own
// colour.
constexpr void ansi_decoration_set_line_color(TextDecoration& decoration, u16 color) noexcept {
    if (color == kAnsiColorNone)
        return;
    decoration.reserved0 = static_cast<u8>(color - 1u);
    decoration.reserved1 = 2;
}
constexpr u16 ansi_decoration_line_color(const TextDecoration& decoration) noexcept {
    return (decoration.kind == TextDecorationKind::underline ||
            decoration.kind == TextDecorationKind::strikethrough) &&
                   decoration.reserved1 == 2
               ? static_cast<u16>(decoration.reserved0 + 1u)
               : kAnsiColorNone;
}

// Paint: tags each glyph's TextGlyph::reserved with its foreground code (0
// for none) from the styles over source, through the shaped cluster map. A
// cluster takes its first unit's colour and all of its glyphs share it. False,
// without writing, when no span has a foreground or the map is unavailable
// (cached shapes keep none). Glyph effects also lend reserved: never combine.
bool ansi_text_tag_glyphs(const ShapedText& text, Utf8View source, ConstSlice<TextStyleSpan> styles) noexcept;

// Index 0-15 resolve through the theme's ansi tokens; 16-255 are fixed.
Color ansi_palette_color(const Theme& theme, u8 index) noexcept;
// The nearest fixed xterm colour (16-255) by squared RGB distance.
u8 ansi_nearest_fixed_color(u32 red, u32 green, u32 blue) noexcept;

} // namespace maicord
