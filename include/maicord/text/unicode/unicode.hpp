#pragma once
#include "maicord/text/unicode/generated/unicode_data.hpp"

namespace maicord {
enum class UnicodeBracketType : u8 { none,open,close };
struct UnicodeBracket { u32 paired;UnicodeBracketType type; };

// Unicode 17.0.0 property data, not the bidi/segmentation/shaping algorithms.
// All code points 0..10FFFF have records, including unassigned, noncharacters
// and surrogate code points. UTF decoders must enforce scalar validity.
// Returns nullptr outside that range. Returned storage is immutable/static.
// The records of code points 0..7F stay constants; everything else comes from tables inflated on first use.
extern const u8 unicode_ascii_index[128];
extern const UnicodeProperties unicode_ascii_records[];
const UnicodeProperties* unicode_properties_beyond_ascii(u32 code_point) noexcept;
inline const UnicodeProperties* unicode_properties(u32 code_point) noexcept {
    return code_point<128?&unicode_ascii_records[unicode_ascii_index[code_point]]:unicode_properties_beyond_ascii(code_point);
}
constexpr bool unicode_is_scalar(u32 code_point) noexcept {
    return code_point<=0x10ffff && !(code_point>=0xd800 && code_point<=0xdfff);
}
// A missing mapping returns the input code point; bracket type is then none.
// Bidi_Mirrored can be true without a code-point mirror mapping; glyph-level
// mirroring in the font remains a separate shaping responsibility.
u32 unicode_bidi_mirror(u32 code_point) noexcept;
UnicodeBracket unicode_bidi_bracket(u32 code_point) noexcept;
// HarfBuzz's modified combining class (_hb_modified_combining_class), the key of its mark reordering: Hebrew and
// Arabic fixed-position classes in shaping order, Telugu length marks and Thai below vowels before viramas, Tibetan
// sign u before i. Zero for a character that is not a mark.
u8 unicode_modified_combining_class(u32 code_point) noexcept;
// One canonical decomposition step (Hangul syllables algorithmically; CJK compatibility ideographs left out): the
// first part, the second through *second (0 for a singleton), or 0 when the code point does not decompose.
u32 unicode_canonical_decompose(u32 code_point,u32* second) noexcept;
// The primary composite of first and second (no composition exclusion), or 0.
u32 unicode_canonical_compose(u32 first,u32 second) noexcept;
// A variation selector that joins its base through cmap format 14 in HarfBuzz's normalization (VS1-VS256); the
// Mongolian free variation selectors are left to shaping (hb-unicode.hh, is_variation_selector).
constexpr bool unicode_layout_variation_selector(u32 code_point) noexcept {
    return code_point-0xfe00u<16 || code_point-0xe0100u<240;
}
} // namespace maicord
