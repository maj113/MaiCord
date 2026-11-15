#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct Utf8DecodeResult {
    u32 codepoint;
    u8 width;
    bool valid;
};

struct Utf8Validation {
    // The scalars counted when valid, else the offset of the first malformed byte: 8 bytes, so a result returns in
    // a register.
    union {
        u32 codepoints;
        u32 error_offset;
    };
    bool valid;
};

Utf8DecodeResult utf8_decode_one(const char* input, u32 available) noexcept;
// The width (2 to 4) of the well-formed sequence at data, whose first byte is 0x80 or above, or 0 when the
// bytes there are not one. It reads at most `available` bytes (1 at least). utf8_decode_one accepts the same
// sequences: this form gives no scalar, for a caller that only validates or copies.
inline u32 utf8_sequence_width(const char* data, u32 available) noexcept {
    const u8 first = static_cast<u8>(data[0]);
    const u8 second = available > 1 ? static_cast<u8>(data[1]) : 0;
    if (first < 0xE0) return first >= 0xC2 && (second & 0xC0) == 0x80 ? 2 : 0;
    if (first < 0xF0) {
        // E0 A0..BF is the shortest form. ED A0..BF is a surrogate.
        const u8 low = first == 0xE0 ? 0xA0 : 0x80, high = first == 0xED ? 0x9F : 0xBF;
        return available >= 3 && second >= low && second <= high && (static_cast<u8>(data[2]) & 0xC0) == 0x80 ? 3 : 0;
    }
    // F0 90..BF is the shortest form. F4 8F is the last plane.
    const u8 low = first == 0xF0 ? 0x90 : 0x80, high = first == 0xF4 ? 0x8F : 0xBF;
    return first <= 0xF4 && available >= 4 && second >= low && second <= high &&
                   (static_cast<u8>(data[2]) & 0xC0) == 0x80 && (static_cast<u8>(data[3]) & 0xC0) == 0x80
               ? 4 : 0;
}
Utf8Validation utf8_validate(const Utf8View& input) noexcept;
// Copies whole scalars from an ACP range. A too-small output returns a prefix
// and its next ACP position; malformed/split endpoints fail before writing.
bool utf8_copy_utf16_range(const Utf8View& text, u32 start, u32 end, const Slice<u16>& output,
    u32* written, u32* next) noexcept;
// Client word-selection policy (double-click): the run containing the scalar
// at `position` of one class: whitespace (incl. no-break/ideographic spaces),
// word characters (letters, digits, underscore, other scripts), punctuation
// and symbols, Han, Hiragana or Katakana runs, or a single emoji cluster.
// Combining marks/joiners/variation selectors extend their base. A position
// on a line break selects the preceding run on that line (nothing on an empty
// line); a selection never crosses a line break.
bool utf8_word_range(const Utf8View& text, u32 position, u32* first, u32* last) noexcept;
// Pointer variant for rounded caret positions: a position whose scalar is
// whitespace/a line break (or the end) right after a non-space run selects
// that run, so clicking the trailing half of a word's last letter selects it.
bool utf8_word_range_near(const Utf8View& text, u32 position, u32* first, u32* last) noexcept;
// Triple-click: the line/paragraph containing `position`, without its break.
bool utf8_line_range(const Utf8View& text, u32 position, u32* first, u32* last) noexcept;
bool utf8_word_boundary(const Utf8View& text, u32 position, bool forward, u32* boundary) noexcept;
u8 utf8_encode(u32 codepoint, char output[4]) noexcept;
bool utf8_utf16_position_for_byte_offset(const Utf8View& text, u32 byte_offset,
                                         u32* utf16_position) noexcept;
bool utf8_byte_offset_for_utf16_position(const Utf8View& text, u32 utf16_position,
                                         u32* byte_offset) noexcept;
bool utf8_utf16_position_for_codepoint(const Utf8View& text, u32 codepoint_index,
                                       u32* utf16_position) noexcept;
bool utf8_codepoint_for_utf16_position(const Utf8View& text, u32 utf16_position,
                                       u32* codepoint_index) noexcept;
// UTF-16 code units of text, assumed valid: one per scalar, two from a
// four-byte sequence.
u32 utf8_utf16_units(const Utf8View& text) noexcept;
// offset is within text and does not split a UTF-8 sequence.
bool utf8_is_boundary(const Utf8View& text, u32 offset) noexcept;

// The longest prefix of text within maximum_bytes that does not split a
// UTF-8 sequence.
constexpr u32 utf8_truncated_length(const Utf8View& text, u32 maximum_bytes) noexcept {
    u32 bytes = text.bytes < maximum_bytes ? text.bytes : maximum_bytes;
    if (bytes < text.bytes)
        while (bytes && (static_cast<u8>(text.data[bytes]) & 0xC0) == 0x80)
            --bytes;
    return bytes;
}

// Copies that prefix into destination and returns its length. Text without data copies nothing.
__declspec(noinline) inline u32 utf8_copy_truncated(char* destination, u32 capacity,
                                                    const Utf8View& text) noexcept {
    const u32 bytes = text.data ? utf8_truncated_length(text, capacity) : 0;
    copy_bytes(destination, text.data, bytes);
    return bytes;
}

template <u32 Capacity> __forceinline u32 utf8_copy_truncated(char (&destination)[Capacity], const Utf8View& text) noexcept {
    return utf8_copy_truncated(destination, Capacity, text);
}

// Copies text into a fixed field and stores the copied length in the field's byte count.
template <u32 Capacity, typename Count>
__forceinline void utf8_assign(char (&destination)[Capacity], Count& bytes, const Utf8View& text) noexcept {
    bytes = static_cast<Count>(utf8_copy_truncated(destination, Capacity, text));
}

} // namespace maicord
