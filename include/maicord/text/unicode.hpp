#pragma once
#include "maicord/text/generated/unicode_data.hpp"

namespace maicord {
enum class UnicodeBracketType : u8 { none,open,close };
struct UnicodeBracket { u32 paired;UnicodeBracketType type; };

// Unicode 17.0.0 property data, not the bidi/segmentation/shaping algorithms.
// All code points 0..10FFFF have records, including unassigned, noncharacters
// and surrogate code points. UTF decoders must enforce scalar validity.
// Returns nullptr outside that range. Returned storage is immutable/static.
const UnicodeProperties* unicode_properties(u32 code_point) noexcept;
constexpr bool unicode_is_scalar(u32 code_point) noexcept {
    return code_point<=0x10ffff && !(code_point>=0xd800 && code_point<=0xdfff);
}
// ISO 15924 tags for reference tools; distinct from OpenType script tags.
u32 unicode_script_tag(UnicodeScript script) noexcept;
// A missing mapping returns the input code point; bracket type is then none.
// Bidi_Mirrored can be true without a code-point mirror mapping; glyph-level
// mirroring in the font remains a separate shaping responsibility.
u32 unicode_bidi_mirror(u32 code_point) noexcept;
UnicodeBracket unicode_bidi_bracket(u32 code_point) noexcept;
usize unicode_property_data_bytes() noexcept;
} // namespace maicord
