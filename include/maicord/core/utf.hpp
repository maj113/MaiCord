#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct Utf8DecodeResult {
    u32 codepoint;
    u8 width;
    bool valid;
};

struct Utf8Validation {
    u32 codepoints;
    u32 error_offset;
    bool valid;
};

Utf8DecodeResult utf8_decode_one(const char* input, u32 available) noexcept;
Utf8Validation utf8_validate(Utf8View input) noexcept;
// Copies whole scalars from an ACP range. A too-small output returns a prefix
// and its next ACP position; malformed/split endpoints fail before writing.
bool utf8_copy_utf16_range(Utf8View text, u32 start, u32 end, Slice<u16> output,
    u32* written, u32* next) noexcept;
// Client word-selection policy (double-click): the run containing the scalar
// at `position` of one class: whitespace (incl. no-break/ideographic spaces),
// word characters (letters, digits, underscore, other scripts), punctuation
// and symbols, Han, Hiragana or Katakana runs, or a single emoji cluster.
// Combining marks/joiners/variation selectors extend their base. A position
// on a line break selects the preceding run on that line (nothing on an empty
// line); a selection never crosses a line break.
bool utf8_word_range(Utf8View text, u32 position, u32* first, u32* last) noexcept;
// Pointer variant for rounded caret positions: a position whose scalar is
// whitespace/a line break (or the end) right after a non-space run selects
// that run, so clicking the trailing half of a word's last letter selects it.
bool utf8_word_range_near(Utf8View text, u32 position, u32* first, u32* last) noexcept;
// Triple-click: the line/paragraph containing `position`, without its break.
bool utf8_line_range(Utf8View text, u32 position, u32* first, u32* last) noexcept;
bool utf8_word_boundary(Utf8View text, u32 position, bool forward, u32* boundary) noexcept;
u8 utf8_encode(u32 codepoint, char output[4]) noexcept;
bool utf8_utf16_position_for_byte_offset(Utf8View text, u32 byte_offset,
                                         u32* utf16_position) noexcept;
bool utf8_byte_offset_for_utf16_position(Utf8View text, u32 utf16_position,
                                         u32* byte_offset) noexcept;
bool utf8_utf16_position_for_codepoint(Utf8View text, u32 codepoint_index,
                                       u32* utf16_position) noexcept;
bool utf8_codepoint_for_utf16_position(Utf8View text, u32 utf16_position,
                                       u32* codepoint_index) noexcept;
// UTF-16 code units of text, assumed valid: one per scalar, two from a
// four-byte sequence.
u32 utf8_utf16_units(Utf8View text) noexcept;
// offset is within text and does not split a UTF-8 sequence.
bool utf8_is_boundary(Utf8View text, u32 offset) noexcept;

constexpr bool utf8_is_continuation(char byte) noexcept {
    return (static_cast<u8>(byte) & 0xC0) == 0x80;
}

// The longest prefix of text within maximum_bytes that does not split a
// UTF-8 sequence.
constexpr u32 utf8_truncated_length(Utf8View text, u32 maximum_bytes) noexcept {
    u32 bytes = text.bytes < maximum_bytes ? text.bytes : maximum_bytes;
    if (bytes < text.bytes)
        while (bytes && (static_cast<u8>(text.data[bytes]) & 0xC0) == 0x80)
            --bytes;
    return bytes;
}

// Copies that prefix into destination and returns its length.
inline u32 utf8_copy_truncated(char* destination, u32 capacity, Utf8View text) noexcept {
    const u32 bytes = utf8_truncated_length(text, capacity);
    copy_bytes(destination, text.data, bytes);
    return bytes;
}

template <u32 Capacity> u32 utf8_copy_truncated(char (&destination)[Capacity], Utf8View text) noexcept {
    return utf8_copy_truncated(destination, Capacity, text);
}

} // namespace maicord
