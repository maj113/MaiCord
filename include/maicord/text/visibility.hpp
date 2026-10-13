#pragma once
#include "maicord/text/base.hpp"
namespace maicord {
// Windows-compatible non-visual item classification. This is
// distinct from Default_Ignorable, glyph coverage, script resolution and bidi.
// Layout still handles tabs, hard breaks and discretionary soft hyphens.
// False for invalid scalars. No Unicode table, allocation or OS call is needed.
bool native_text_nonvisual(u32 scalar) noexcept;
// Explicit bidi embeddings/overrides/PDF and isolates/PDI. They retain source
// positions and affect analysis but publish no glyphs or advance of their own.
constexpr bool native_text_bidi_format(u32 scalar) noexcept {return scalar-0x202au<5||scalar-0x2066u<4;}
// Boundary-neutral (bidi BN) C0 controls and DEL: U+0001-U+0008,
// U+000E-U+001B and U+007F. Browsers draw them as nothing (pasted ANSI escapes
// stay invisible), as DirectWrite's no-visual items do.
// Like ZWSP they become source-preserving zero-glyph paragraph clusters. Tab,
// line/paragraph separators (VT, FF, U+001C-U+001F) and NUL are excluded.
constexpr bool native_text_zero_width_control(u32 scalar) noexcept {
    return scalar-1u<8u||scalar-0x0eu<14u||scalar==0x7fu;
}
// Default_Ignorable code points are invisible in browsers, but the
// Windows-compatible shapers draw some of them as .notdef or with an
// advance: unconsumed variation selectors (FE0F after a non-emoji base), Hangul
// fillers outside conjoining jamo, Khmer inherent vowels, Mongolian FVS outside
// Mongolian, tag characters outside an emoji tag sequence, and unassigned
// ignorables. Copies input to output, replacing each such scalar by an
// invisible one of identical UTF-8 and UTF-16 length (U+034F, U+200C or
// U+034F U+034F), so styles, offsets and cluster maps stay valid; input is left
// untouched. Returns 0 without writing when nothing needs hiding, the output
// byte count otherwise, or ~0u when output is smaller than input.
u32 text_hide_default_ignorables(Utf8View input,Slice<char> output) noexcept;
// A UTF-8 lead byte that can start a scalar text_hide_default_ignorables hides
// (or replaces); text without one is returned unchanged without decoding.
constexpr bool text_hide_candidate_lead(u8 b) noexcept {return b==0xc2||b==0xd8||b==0xe1||b==0xe2||b==0xe3||b==0xef||b==0xf0||b==0xf3;}
// Process-wide presentation policy, off by default. When enabled, callers hide
// default-ignorables as browsers do and shape a same-length substitute rather
// than failing an item the font cannot express. The shaping engine itself stays
// faithful to DirectWrite (its corpus tests compare against DirectWrite).
void text_set_browser_invisibles(bool enabled) noexcept;
bool text_browser_invisibles() noexcept;
}
