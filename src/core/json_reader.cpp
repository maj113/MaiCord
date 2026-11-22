#include "maicord/core/json_reader.hpp"
#include "maicord/core/ascii.hpp"

#include "maicord/core/intrinsics.hpp"
#include "maicord/sys/simd_haswell.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {

struct IndexedReader {
    Utf8View input;
    ConstSlice<u32> structurals;
    u32 offset;
    u32 structural_index;
    u32 maximum_depth;
    u32 error_offset;
    JsonReadError error;
    // Null, or the table of json_index_ends from the first structural of this reader on. Its entries count
    // from the start of the document's index.
    const u32* ends;
    // Null, or the room where a checked visit writes that table for each container it validates. deepest: the
    // depth of the deepest container closed so far, which bounds the levels of every later one.
    u32* fill;
    u32 deepest;
};

// A member visitor (object visits) or an element visitor (array visits) as one type; `object` says which.
using AnyVisitor = void (*)() noexcept;

bool fail(IndexedReader* reader, JsonReadError error, u32 offset) noexcept {
    if (reader->error == JsonReadError::none) {
        reader->error = error;
        reader->error_offset = offset;
    }
    return false;
}

__forceinline bool is_whitespace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

__forceinline void skip_whitespace(IndexedReader* reader) noexcept {
    while (reader->offset < reader->input.bytes && is_whitespace(reader->input.data[reader->offset])) {
        ++reader->offset;
    }
}

inline void skip_optional_utf8_bom(IndexedReader* reader) noexcept {
    if (reader->input.bytes >= 3 && static_cast<u8>(reader->input.data[0]) == 0xEF &&
        static_cast<u8>(reader->input.data[1]) == 0xBB &&
        static_cast<u8>(reader->input.data[2]) == 0xBF) {
        reader->offset = 3;
    }
}

__forceinline bool consume_structural(IndexedReader* reader, char expected, JsonReadError error) noexcept {
    if (reader->offset >= reader->input.bytes || reader->input.data[reader->offset] != expected) {
        return fail(reader, error, reader->offset);
    }
    if (reader->structural_index >= reader->structurals.count ||
        reader->structurals[reader->structural_index] != reader->offset) {
        return fail(reader, JsonReadError::structural_index_mismatch, reader->offset);
    }
    ++reader->offset;
    ++reader->structural_index;
    return true;
}

bool read_hex_quad(const char* data, u32 offset, u32 end, u32* value) noexcept {
    if (end - offset < 4) {
        return false;
    }
    u32 result = 0;
    for (u32 index = 0; index < 4; ++index) {
        const i32 digit = ascii_hex_value(data[offset + index]);
        if (digit < 0) {
            return false;
        }
        result = (result << 4) | static_cast<u32>(digit);
    }
    *value = result;
    return true;
}

// The character of each one-letter escape, indexed by the letter minus '"' (the first of them); 0 for none.
struct ShortEscapes {
    u8 character['t' - '"' + 1];
};
constexpr ShortEscapes make_short_escapes() noexcept {
    ShortEscapes escapes{};
    escapes.character['"' - '"'] = '"';
    escapes.character['\\' - '"'] = '\\';
    escapes.character['/' - '"'] = '/';
    escapes.character['b' - '"'] = 0x08;
    escapes.character['f' - '"'] = 0x0C;
    escapes.character['n' - '"'] = 0x0A;
    escapes.character['r' - '"'] = 0x0D;
    escapes.character['t' - '"'] = 0x09;
    return escapes;
}
constexpr ShortEscapes kShortEscapes = make_short_escapes();

// The character the one-letter escape `letter` stands for, or 0 when it is not one.
__forceinline u32 short_escape(char letter) noexcept {
    const u32 slot = static_cast<u32>(static_cast<u8>(letter)) - '"';
    return slot < sizeof(kShortEscapes.character) ? kShortEscapes.character[slot] : 0;
}

// The scalar of the \u escape whose hex digits start at data[at] (a high surrogate takes the next \u escape too)
// and the offset after it; 0 for a malformed one, with *error_at the offset of the digits at fault.
__forceinline u32 unicode_escape(const char* data, u32 at, u32 end, u32* codepoint, u32* error_at) noexcept {
    u32 first = 0;
    *error_at = at;
    if (!read_hex_quad(data, at, end, &first) || (first >= 0xDC00 && first <= 0xDFFF)) {
        return 0;
    }
    if (first < 0xD800 || first > 0xDBFF) {
        *codepoint = first;
        return at + 4;
    }
    *error_at = at + 4;
    if (end - (at + 4) < 6 || data[at + 4] != '\\' || data[at + 5] != 'u') {
        return 0;
    }
    u32 second = 0;
    *error_at = at + 6;
    if (!read_hex_quad(data, at + 6, end, &second) || second < 0xDC00 || second > 0xDFFF) {
        return 0;
    }
    *codepoint = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
    return at + 10;
}

// Bytes that need the scalar path: controls, non-ASCII (negative as signed
// bytes) and backslashes. Plain ASCII runs advance 16 bytes at a time.
__forceinline u32 special_bytes(const char* data) noexcept {
    const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
    const __m128i special = _mm_or_si128(_mm_cmplt_epi8(bytes, _mm_set1_epi8(0x20)),
                                         _mm_cmpeq_epi8(bytes, _mm_set1_epi8('\\')));
    return static_cast<u32>(_mm_movemask_epi8(special));
}

// The low `count` (0..16) bits.
__forceinline u32 low_bits(u32 count) noexcept {
    return 0xFFFFu >> (16 - count);
}

// special_bytes for 32 bytes.
MAICORD_HASWELL __forceinline u32 wide_special_bytes(const char* data) noexcept {
    const __m256i bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(data));
    return static_cast<u32>(_mm256_movemask_epi8(_mm256_or_si256(
        _mm256_cmpgt_epi8(_mm256_set1_epi8(0x20), bytes), _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('\\')))));
}

MAICORD_HASWELL bool validate_string_content(IndexedReader* reader, u32 begin, u32 end, bool* has_escapes) noexcept {
    u32 offset = begin;
    bool escaped = false;
    while (offset < end) {
        // Any 32 bytes inside the input may be loaded; those past the string are masked off.
        if (reader->input.bytes - offset >= 32) {
            const u32 left = end - offset;
            u32 special = wide_special_bytes(reader->input.data + offset);
            if (left < 32) special &= (1u << left) - 1;
            if (!special) {
                offset += left < 32 ? left : 32;
                continue;
            }
            offset += lowest_set_bit(special);
        }
        const u8 value = static_cast<u8>(reader->input.data[offset]);
        if (value < 0x20) {
            return fail(reader, JsonReadError::invalid_string, offset);
        }
        if (value < 0x80 && value != '\\') {
            ++offset;
            continue;
        }
        if (value != '\\') {
            const Utf8DecodeResult decoded = utf8_decode_one(reader->input.data + offset, end - offset);
            if (!decoded.valid) {
                return fail(reader, JsonReadError::invalid_utf8, offset);
            }
            offset += decoded.width;
            continue;
        }

        escaped = true;
        ++offset;
        if (offset >= end) {
            return fail(reader, JsonReadError::invalid_escape, offset - 1);
        }
        const char escape = reader->input.data[offset++];
        if (short_escape(escape)) {
            continue;
        }
        if (escape != 'u') {
            return fail(reader, JsonReadError::invalid_escape, offset - 1);
        }
        u32 scalar = 0, error_at = 0;
        offset = unicode_escape(reader->input.data, offset, end, &scalar, &error_at);
        if (!offset) {
            return fail(reader, JsonReadError::invalid_unicode_escape, error_at);
        }
    }
    *has_escapes = escaped;
    return true;
}

// Whether the eight bytes of word hold a backslash (0x5C): a zero byte in word XOR 0x5C in every byte. The test is
// exact for the whole word: a false hit can only come after a true one.
__forceinline bool word_has_backslash(u64 word) noexcept {
    const u64 x = word ^ 0x5C5C'5C5C'5C5C'5C5Cull;
    return ((x - 0x0101'0101'0101'0101ull) & ~x & 0x8080'8080'8080'8080ull) != 0;
}

// Whether data[begin, end) holds a backslash; `readable` bytes of data may be loaded.
__forceinline bool has_backslash(const char* data, u32 begin, u32 end, u32 readable) noexcept {
    for (u32 offset = begin; offset < end; offset += 16) {
        if (readable - offset < 16) {
            // Words inside [offset, end) only: overlapping loads of 8 or 4 bytes, else single bytes.
            const auto load = [&](u32 at, u32 width) noexcept {
                return width == 8 ? *reinterpret_cast<const u64 __unaligned*>(data + at)
                                  : *reinterpret_cast<const u32 __unaligned*>(data + at);
            };
            const u32 left = end - offset;
            if (left >= 8)
                return word_has_backslash(load(offset, 8)) || word_has_backslash(load(end - 8, 8));
            if (left >= 4)
                return word_has_backslash(load(offset, 4) | load(end - 4, 4) << 32);
            for (; offset < end; ++offset)
                if (data[offset] == '\\') return true;
            return false;
        }
        u32 mask = static_cast<u32>(_mm_movemask_epi8(
            _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + offset)), _mm_set1_epi8('\\'))));
        if (end - offset < 16) mask &= low_bits(end - offset);
        if (mask) return true;
    }
    return false;
}

bool parse_string(IndexedReader* reader, JsonString* output, JsonReadError expected_error) noexcept {
    const u32 opening_offset = reader->offset;
    if (!consume_structural(reader, '"', expected_error)) {
        return false;
    }
    const u32 content_begin = reader->offset;
    if (reader->structural_index >= reader->structurals.count) {
        return fail(reader, JsonReadError::structural_index_mismatch, content_begin);
    }
    const u32 closing_offset = reader->structurals[reader->structural_index];
    if (closing_offset < content_begin || closing_offset >= reader->input.bytes ||
        reader->input.data[closing_offset] != '"') {
        return fail(reader, JsonReadError::structural_index_mismatch, content_begin);
    }

    bool has_escapes = false;
    // Most strings are short and plain: one masked 16-byte check settles them without the call.
    const u32 length = closing_offset - content_begin;
    const bool plain = length <= 16 && reader->input.bytes - content_begin >= 16 &&
                       !(special_bytes(reader->input.data + content_begin) & low_bits(length));
    if (!plain && !validate_string_content(reader, content_begin, closing_offset, &has_escapes)) {
        return false;
    }
    reader->offset = closing_offset + 1;
    ++reader->structural_index;
    if (output) {
        output->encoded = Utf8View{reader->input.data + content_begin, closing_offset - content_begin};
        output->source_offset = opening_offset + 1;
        output->has_escapes = has_escapes;
    }
    return true;
}

// The end of the number that starts at data[offset], or offset when the text there is not one.
u32 number_end(const char* data, u32 offset, u32 bytes) noexcept {
    const u32 begin = offset;
    if (offset < bytes && data[offset] == '-') {
        ++offset;
    }
    if (offset >= bytes) {
        return begin;
    }
    if (data[offset] == '0') {
        ++offset;
        if (offset < bytes && data[offset] >= '0' && data[offset] <= '9') {
            return begin;
        }
    } else {
        if (data[offset] < '1' || data[offset] > '9') {
            return begin;
        }
        do {
            ++offset;
        } while (offset < bytes && data[offset] >= '0' && data[offset] <= '9');
    }
    if (offset < bytes && data[offset] == '.') {
        ++offset;
        const u32 fraction_begin = offset;
        while (offset < bytes && data[offset] >= '0' && data[offset] <= '9') {
            ++offset;
        }
        if (offset == fraction_begin) {
            return begin;
        }
    }
    if (offset < bytes && (data[offset] == 'e' || data[offset] == 'E')) {
        ++offset;
        if (offset < bytes && (data[offset] == '+' || data[offset] == '-')) {
            ++offset;
        }
        const u32 exponent_begin = offset;
        while (offset < bytes && data[offset] >= '0' && data[offset] <= '9') {
            ++offset;
        }
        if (offset == exponent_begin) {
            return begin;
        }
    }
    return offset;
}

__forceinline bool is_value_end(char value) noexcept {
    return is_whitespace(value) || value == ',' || value == ']' || value == '}';
}

constexpr u32 word(char a, char b, char c, char d) noexcept {
    return static_cast<u8>(a) | static_cast<u32>(static_cast<u8>(b)) << 8 | static_cast<u32>(static_cast<u8>(c)) << 16 |
           static_cast<u32>(static_cast<u8>(d)) << 24;
}

// The end of the well-formed number, true, false or null at data[begin] (a non-blank byte), whose token
// ends the input or is followed by a value end; 0 for anything else.
__forceinline u32 scalar_end(const char* data, u32 begin, u32 bytes, JsonValueKind* kind) noexcept {
    const u32 left = bytes - begin;
    u32 end = 0;
    const char first = data[begin];
    if (first == 't' || first == 'f' || first == 'n') {
        const u32 text = left >= 4 ? *reinterpret_cast<const u32 __unaligned*>(data + begin) : 0;
        if (text == word('t', 'r', 'u', 'e')) {
            end = begin + 4;
            *kind = JsonValueKind::boolean;
        } else if (text == word('n', 'u', 'l', 'l')) {
            end = begin + 4;
            *kind = JsonValueKind::null_value;
        } else if (text == word('f', 'a', 'l', 's') && left >= 5 && data[begin + 4] == 'e') {
            end = begin + 5;
            *kind = JsonValueKind::boolean;
        } else {
            return 0;
        }
    } else {
        end = number_end(data, begin, bytes);
        if (end == begin) {
            return 0;
        }
        *kind = JsonValueKind::number;
    }
    return end == bytes || is_value_end(data[end]) ? end : 0;
}

bool skip_value(IndexedReader* reader, u32 depth, JsonValue* output) noexcept;

// The exact search for the offending bracket, run only when the container is malformed or too deep.
bool skip_container_slow(IndexedReader* reader, u32 depth) noexcept {
    u32 level = 0;
    for (u32 index = reader->structural_index; index < reader->structurals.count; ++index) {
        const u32 at = reader->structurals[index];
        const char value = reader->input.data[at];
        if (value == '{' || value == '[') {
            if (depth + level > reader->maximum_depth) return fail(reader, JsonReadError::depth_limit, at);
            ++level;
        } else if ((value == '}' || value == ']') && --level == 0) {
            reader->offset = at + 1;
            reader->structural_index = index + 1;
            return true;
        }
    }
    return fail(reader, JsonReadError::structural_index_mismatch, reader->offset);
}

// +1 for { and [, -1 for } and ], 0 for the other indexed bytes.
struct NestingSteps {
    signed char step[256];
};
constexpr NestingSteps make_nesting_steps() noexcept {
    NestingSteps steps{};
    steps.step['{'] = steps.step['['] = 1;
    steps.step['}'] = steps.step[']'] = -1;
    return steps;
}
constexpr NestingSteps kNesting = make_nesting_steps();

// Validates the container at the reader's position, which `open` starts, and everything inside it. The position
// and the open containers stay in locals: nested containers do not recurse, up to 64 levels for each call.
MAICORD_HASWELL bool skip_composite(IndexedReader* reader, u32 depth, char open) noexcept {
    const char* const data = reader->input.data;
    const u32 bytes = reader->input.bytes;
    const u32* const offsets = reader->structurals.data;
    const u32 total = reader->structurals.count;
    const u32 maximum_depth = reader->maximum_depth;
    u32* const fill = reader->fill;
    u32 offset = reader->offset, index = reader->structural_index;
    // The open containers of this call: one bit for each (set for an object, the lowest bit the innermost)
    // and the index position of each opening bracket.
    u64 objects = 0;
    u32 level = 0;
    u32 opened[64];
    const auto blanks = [&]() noexcept {
        while (offset < bytes && is_whitespace(data[offset])) ++offset;
    };
    // The indexed byte `expected` at the position.
    const auto structural = [&](char expected, JsonReadError error) __attribute__((always_inline)) noexcept {
        if (offset >= bytes || data[offset] != expected) return fail(reader, error, offset);
        if (index >= total || offsets[index] != offset)
            return fail(reader, JsonReadError::structural_index_mismatch, offset);
        ++offset;
        ++index;
        return true;
    };
    // The string at the position. Inline at both uses, so the position stays in registers.
    const auto string = [&](JsonReadError error) MAICORD_HASWELL __attribute__((always_inline)) noexcept {
        if (!structural('"', error)) return false;
        if (index >= total) return fail(reader, JsonReadError::structural_index_mismatch, offset);
        const u32 closing = offsets[index];
        if (closing < offset || closing >= bytes || data[closing] != '"')
            return fail(reader, JsonReadError::structural_index_mismatch, offset);
        // Most strings are short and plain: one masked 32-byte check settles them without the call.
        const u32 length = closing - offset;
        bool has_escapes;
        if (!(length <= 32 && bytes - offset >= 32 &&
              !(wide_special_bytes(data + offset) & ((1ull << length) - 1))) &&
            !validate_string_content(reader, offset, closing, &has_escapes))
            return false;
        offset = closing + 1;
        ++index;
        return true;
    };
enter:
    // A container starts at the position: `open` at `depth`.
    if (level == 64) {
        reader->offset = offset;
        reader->structural_index = index;
        if (!skip_composite(reader, depth, open)) return false;
        offset = reader->offset;
        index = reader->structural_index;
        --depth;
        goto value_done;
    }
    if (depth > maximum_depth) return fail(reader, JsonReadError::depth_limit, offset);
    opened[level++] = index;
    objects = objects << 1 | (open == '{');
    if (!structural(open, JsonReadError::expected_value)) return false;
    blanks();
    if (offset < bytes && data[offset] == (objects & 1 ? '}' : ']')) goto close;
member:
    if (objects & 1) {
        if (!string(JsonReadError::expected_member_name)) return false;
        blanks();
        if (!structural(':', JsonReadError::expected_colon)) return false;
    }
    blanks();
    if (offset >= bytes) return fail(reader, JsonReadError::expected_value, offset);
    {
        const char first = data[offset];
        if (first == '{' || first == '[') {
            open = first;
            ++depth;
            goto enter;
        }
        if (first == '"') {
            if (!string(JsonReadError::expected_value)) return false;
        } else {
            JsonValueKind kind;
            const u32 end = scalar_end(data, offset, bytes, &kind);
            if (!end) {
                const bool number_like = first == '-' || (first >= '0' && first <= '9');
                return fail(reader, number_like ? JsonReadError::invalid_number : JsonReadError::invalid_literal,
                            offset);
            }
            offset = end;
        }
    }
value_done:
    // A value of the innermost container ended. `depth` is the depth of that container.
    blanks();
    if (!(offset < bytes && data[offset] == (objects & 1 ? '}' : ']'))) {
        if (!structural(',', JsonReadError::expected_comma_or_end)) return false;
        blanks();
        goto member;
    }
close:
    if (!structural(objects & 1 ? '}' : ']', JsonReadError::expected_comma_or_end)) return false;
    if (fill) {
        if (depth > reader->deepest) reader->deepest = depth;
        fill[opened[level - 1]] = index - 1;
        fill[index - 1] = reader->deepest - depth + 1;
    }
    objects >>= 1;
    if (!--level) {
        reader->offset = offset;
        reader->structural_index = index;
        return true;
    }
    --depth;
    goto value_done;
}

bool skip_value(IndexedReader* reader, u32 depth, JsonValue* output) noexcept {
    skip_whitespace(reader);
    const u32 begin = reader->offset;
    if (begin >= reader->input.bytes) {
        return fail(reader, JsonReadError::expected_value, begin);
    }

    JsonValueKind kind = JsonValueKind::null_value;
    const char first = reader->input.data[begin];
    if (first == '{' || first == '[') {
        kind = first == '{' ? JsonValueKind::object : JsonValueKind::array;
        if (!skip_composite(reader, depth, first)) {
            return false;
        }
    } else if (first == '"') {
        kind = JsonValueKind::string;
        if (!parse_string(reader, nullptr, JsonReadError::expected_value)) {
            return false;
        }
    } else {
        const u32 end = scalar_end(reader->input.data, begin, reader->input.bytes, &kind);
        if (!end) {
            const bool number_like = first == '-' || (first >= '0' && first <= '9');
            return fail(reader, number_like ? JsonReadError::invalid_number : JsonReadError::invalid_literal, begin);
        }
        reader->offset = end;
    }

    if (output) {
        output->source = Utf8View{reader->input.data + begin, reader->offset - begin};
        output->source_offset = begin;
        output->kind = kind;
        output->validated = true;
    }
    return true;
}

bool decode_string_scalar(const JsonString& value, u32* offset, u32* codepoint) noexcept {
    if (*offset >= value.encoded.bytes) {
        return false;
    }
    const char current = value.encoded.data[*offset];
    if (current != '\\') {
        if (static_cast<u8>(current) < 0x80) {
            ++*offset;
            *codepoint = static_cast<u8>(current);
            return true;
        }
        const Utf8DecodeResult decoded =
            utf8_decode_one(value.encoded.data + *offset, value.encoded.bytes - *offset);
        if (!decoded.valid) {
            return false;
        }
        *offset += decoded.width;
        *codepoint = decoded.codepoint;
        return true;
    }

    if (value.encoded.bytes - *offset < 2) {
        return false;
    }
    const char escape = value.encoded.data[*offset + 1];
    *offset += 2;
    if (const u32 character = short_escape(escape)) {
        *codepoint = character;
        return true;
    }
    if (escape != 'u') {
        return false;
    }
    u32 error_at = 0;
    const u32 next = unicode_escape(value.encoded.data, *offset, value.encoded.bytes, codepoint, &error_at);
    if (!next) {
        return false;
    }
    *offset = next;
    return true;
}

} // namespace

// The members of a validated object, or the elements of a validated array, found from the structural index
// alone: a member's name is two indexed quotes and a colon, a string value two more, and no byte of a string or
// number is examined. The position stays in locals across visitor calls and reaches the reader on exit, where
// the caller continues at the closing bracket. Stops at the first error, which is in the reader.
static void visit_trusted(IndexedReader* reader, bool object, AnyVisitor visitor, void* context, u32* hint,
                          u32 base, u32* members) noexcept {
    const char close = object ? '}' : ']';
    const char* const data = reader->input.data;
    const u32 bytes = reader->input.bytes;
    const u32* const offsets = reader->structurals.data;
    const u32 total = reader->structurals.count;
    u32 offset = reader->offset, index = reader->structural_index, count = 0;
    const auto stop = [&](JsonReadError error, u32 at) noexcept {
        fail(reader, error, at);
        *members = count;
    };
    for (;;) {
        JsonString name{};
        if (object) {
            if (total - index < 3) return stop(JsonReadError::structural_index_mismatch, offset);
            const u32 open = offsets[index], end = offsets[index + 1], colon = offsets[index + 2];
            if (open != offset || end <= open || colon <= end || colon >= bytes)
                return stop(JsonReadError::structural_index_mismatch, offset);
            name.encoded = Utf8View{data + open + 1, end - open - 1};
            name.source_offset = open + 1;
            name.has_escapes = has_backslash(data, open + 1, end, bytes);
            index += 3;
            offset = colon + 1;
        }
        while (offset < bytes && is_whitespace(data[offset])) ++offset;
        if (offset >= bytes) return stop(JsonReadError::expected_value, offset);
        // A nested visit of this member's value starts its index search here.
        if (hint) *hint = base + index;
        const u32 begin = offset;
        const char first = data[begin];
        JsonValueKind kind;
        if (first == '{' || first == '[') {
            kind = first == '{' ? JsonValueKind::object : JsonValueKind::array;
            if (index >= total || offsets[index] != begin) return stop(JsonReadError::structural_index_mismatch, begin);
            // The table gives the closing bracket and the levels of the container. Without it, or for a
            // container that is too deep, the nesting level is followed over the indexed bytes: the deepest
            // level is checked once the container closes, and the exact search runs only for a malformed or
            // too deep one.
            const u32* at = offsets + index;
            const u32* const ends = reader->ends;
            const u32 closer = ends ? ends[index] - base : 0;
            if (ends && closer - index - 1 < total - index - 1 && ends[closer] < reader->maximum_depth) {
                at = offsets + closer;
            } else {
                u32 level = 0, peak = 0;
                const u32* const last = offsets + total;
                for (; at != last; ++at) {
                    level += static_cast<u32>(kNesting.step[static_cast<u8>(data[*at])]);
                    peak = level > peak ? level : peak;
                    if (!level) break;
                }
                if (at == last || peak + 1 > reader->maximum_depth) {
                    reader->offset = begin;
                    reader->structural_index = index;
                    skip_container_slow(reader, 2);
                    return stop(JsonReadError::structural_index_mismatch, begin);
                }
            }
            offset = *at + 1;
            index = static_cast<u32>(at - offsets) + 1;
        } else if (first == '"') {
            kind = JsonValueKind::string;
            if (total - index < 2) return stop(JsonReadError::structural_index_mismatch, begin);
            const u32 open = offsets[index], end = offsets[index + 1];
            if (open != begin || end <= open || end >= bytes)
                return stop(JsonReadError::structural_index_mismatch, begin);
            offset = end + 1;
            index += 2;
        } else {
            // Bounded by the next structural, minus blanks, instead of byte by byte.
            u32 end = bytes;
            if (index < total) {
                const u32 next = offsets[index];
                if (next >= begin && next < end) end = next;
            }
            while (end > begin && is_whitespace(data[end - 1])) --end;
            offset = end;
            kind = first == 't' || first == 'f' ? JsonValueKind::boolean
                   : first == 'n'               ? JsonValueKind::null_value
                                                : JsonValueKind::number;
        }
        const JsonValue value{Utf8View{data + begin, offset - begin}, begin, kind, true};
        if (object) {
            ++count;
            if (visitor && !reinterpret_cast<JsonMemberVisitor>(visitor)(context, name, value))
                return stop(JsonReadError::visitor_rejected, name.source_offset);
        } else {
            if (visitor && !reinterpret_cast<JsonArrayVisitor>(visitor)(context, count, value))
                return stop(JsonReadError::visitor_rejected, value.source_offset);
            ++count;
        }
        while (offset < bytes && is_whitespace(data[offset])) ++offset;
        if (offset < bytes && data[offset] == close) break;
        if (offset >= bytes || data[offset] != ',') return stop(JsonReadError::expected_comma_or_end, offset);
        if (index >= total || offsets[index] != offset) return stop(JsonReadError::structural_index_mismatch, offset);
        ++index;
        ++offset;
        while (offset < bytes && is_whitespace(data[offset])) ++offset;
    }
    reader->offset = offset;
    reader->structural_index = index;
    *members = count;
}

// The loop of a visit of a value that is not validated yet: every member or element is validated as it is visited.
// Stops at the closing bracket or the first error, which is in the reader.
__declspec(noinline) static void visit_checked(IndexedReader* reader, bool object, AnyVisitor visitor, void* context,
                                               u32* hint, u32 base, u32* members) noexcept {
    const char close = object ? '}' : ']';
    u32 count = 0;
    for (;;) {
        JsonString name{};
        if (object) {
            if (!parse_string(reader, &name, JsonReadError::expected_member_name)) {
                break;
            }
            skip_whitespace(reader);
            if (!consume_structural(reader, ':', JsonReadError::expected_colon)) {
                break;
            }
        }
        // A nested visit of this member's value starts its index search here.
        if (hint) *hint = base + reader->structural_index;
        JsonValue value{};
        if (!skip_value(reader, 2, &value)) {
            break;
        }
        if (object) {
            ++count;
            if (visitor && !reinterpret_cast<JsonMemberVisitor>(visitor)(context, name, value)) {
                fail(reader, JsonReadError::visitor_rejected, name.source_offset);
                break;
            }
        } else {
            if (visitor && !reinterpret_cast<JsonArrayVisitor>(visitor)(context, count, value)) {
                fail(reader, JsonReadError::visitor_rejected, value.source_offset);
                break;
            }
            ++count;
        }
        skip_whitespace(reader);
        if (reader->offset < reader->input.bytes && reader->input.data[reader->offset] == close) {
            break;
        }
        if (!consume_structural(reader, ',', JsonReadError::expected_comma_or_end)) {
            break;
        }
        skip_whitespace(reader);
    }
    *members = count;
}

static JsonReadResult visit_range(bool object, Utf8View input, ConstSlice<u32> structurals, u32 maximum_depth,
                                  AnyVisitor visitor, void* context, u32 begin, bool trusted,
                                  const u32* ends = nullptr, u32* hint = nullptr, u32 base = 0,
                                  u32* fill = nullptr) noexcept {
    const char open = object ? '{' : '[';
    const char close = object ? '}' : ']';
    if ((!input.data && input.bytes != 0) || (!structurals.data && structurals.count != 0)) {
        return JsonReadResult{0, JsonReadError::invalid_input};
    }

    IndexedReader reader{input, structurals, begin, 0, maximum_depth, 0, JsonReadError::none, ends, fill, 0};
    if (begin == 0) skip_optional_utf8_bom(&reader);
    skip_whitespace(&reader);
    if (maximum_depth == 0) {
        return JsonReadResult{reader.offset, JsonReadError::depth_limit};
    }
    if (!consume_structural(&reader, open,
                            object ? JsonReadError::expected_root_object : JsonReadError::expected_root_array)) {
        return JsonReadResult{reader.error_offset, reader.error};
    }

    u32 count = 0;
    skip_whitespace(&reader);
    if (reader.offset < input.bytes && input.data[reader.offset] != close) {
        if (trusted) {
            visit_trusted(&reader, object, visitor, context, hint, base, &count);
        } else {
            visit_checked(&reader, object, visitor, context, hint, base, &count);
        }
    }

    if (reader.error == JsonReadError::none) {
        consume_structural(&reader, close, JsonReadError::expected_comma_or_end);
    }
    if (reader.error == JsonReadError::none) {
        skip_whitespace(&reader);
        if (reader.offset != input.bytes) {
            fail(&reader, JsonReadError::trailing_data, reader.offset);
        } else if (!hint && reader.structural_index != structurals.count) {
            fail(&reader, JsonReadError::structural_index_mismatch, reader.offset);
        }
    }
    return JsonReadResult{reader.error == JsonReadError::none ? count : reader.error_offset, reader.error};
}

JsonReadResult json_visit_root_object(const Utf8View& input, const ConstSlice<u32>& structurals,
                                      u32 maximum_depth, JsonMemberVisitor visitor, void* context,
                                      u32* ends) noexcept {
    return visit_range(true, input, structurals, maximum_depth, reinterpret_cast<AnyVisitor>(visitor), context, 0,
                       false, nullptr, nullptr, 0, ends);
}

JsonReadResult json_visit_root_array(const Utf8View& input, const ConstSlice<u32>& structurals,
                                     u32 maximum_depth, JsonArrayVisitor visitor, void* context,
                                     u32* ends) noexcept {
    return visit_range(false, input, structurals, maximum_depth, reinterpret_cast<AnyVisitor>(visitor), context, 0,
                       false, nullptr, nullptr, 0, ends);
}

JsonReadResult json_visit_value_object(const JsonValue& value, const ConstSlice<u32>& structurals,
                                       u32 maximum_depth, JsonMemberVisitor visitor, void* context,
                                       const u32* ends) noexcept {
    return visit_range(true, value.source, structurals, maximum_depth, reinterpret_cast<AnyVisitor>(visitor), context,
                       0, value.validated, ends);
}

JsonReadResult json_visit_value_array(const JsonValue& value, const ConstSlice<u32>& structurals,
                                      u32 maximum_depth, JsonArrayVisitor visitor, void* context) noexcept {
    return visit_range(false, value.source, structurals, maximum_depth, reinterpret_cast<AnyVisitor>(visitor), context,
                       0, value.validated);
}

// The position of the first offset >= value, searched outward from hint: consecutive lookups of a document
// land close together, so a visit of a member's value (the hint) or of a later sibling needs few probes.
static inline u32 structural_find(ConstSlice<u32> offsets, u32 value, u32 hint) noexcept {
    u32 first = 0, last = offsets.count;
    if (hint < last && offsets[hint] < value) {
        first = hint + 1;
        for (u32 step = 1; first - 1 + step < last; step <<= 1) {
            if (offsets[first - 1 + step] >= value) {
                last = first - 1 + step;
                break;
            }
            first += step;
        }
    } else {
        if (hint < last) last = hint;
        for (u32 step = 1; step <= last; step <<= 1) {
            if (offsets[last - step] < value) {
                first = last - step + 1;
                break;
            }
            last -= step;
        }
    }
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (offsets[middle] < value) first = middle + 1;
        else last = middle;
    }
    return first;
}

// The view of one value of a document: the input up to the value's end and the index from the value's first
// structural on. Nothing after the value is consumed, so no end search is needed.
static inline bool indexed_subview(const JsonIndexedDocument& document, const JsonValue& value,
                            Utf8View* prefix, ConstSlice<u32>* offsets, u32* begin, u32* first_index) noexcept {
    if (!document.input.data || !document.structurals.data || !value.source.data || !value.source.bytes)
        return false;
    const auto root = reinterpret_cast<usize>(document.input.data);
    const auto source = reinterpret_cast<usize>(value.source.data);
    if (source < root || source - root > document.input.bytes) return false;
    *begin = static_cast<u32>(source - root);
    if (value.source.bytes > document.input.bytes - *begin) return false;
    *first_index = structural_find(document.structurals, *begin, document.hint);
    *prefix = Utf8View{document.input.data, *begin + value.source.bytes};
    *offsets = ConstSlice<u32>{document.structurals.data + *first_index, document.structurals.count - *first_index};
    return true;
}

static JsonReadResult visit_indexed(bool object, const JsonIndexedDocument& document, const JsonValue& value,
                                    u32 maximum_depth, AnyVisitor visitor, void* context) noexcept {
    Utf8View prefix{};
    ConstSlice<u32> offsets{};
    u32 begin = 0, first = 0;
    if (value.kind != (object ? JsonValueKind::object : JsonValueKind::array) ||
        !indexed_subview(document, value, &prefix, &offsets, &begin, &first))
        return {0, JsonReadError::invalid_input};
    return visit_range(object, prefix, offsets, maximum_depth, visitor, context, begin, value.validated,
                       document.ends ? document.ends + first : nullptr, &document.hint, first);
}

JsonReadResult json_visit_indexed_object(const JsonIndexedDocument& document, const JsonValue& value,
    u32 maximum_depth, JsonMemberVisitor visitor, void* context) noexcept {
    return visit_indexed(true, document, value, maximum_depth, reinterpret_cast<AnyVisitor>(visitor), context);
}

JsonReadResult json_visit_indexed_array(const JsonIndexedDocument& document, const JsonValue& value,
    u32 maximum_depth, JsonArrayVisitor visitor, void* context) noexcept {
    return visit_indexed(false, document, value, maximum_depth, reinterpret_cast<AnyVisitor>(visitor), context);
}

bool json_index_ends(const Utf8View& input, const ConstSlice<u32>& structurals, u32* ends) noexcept {
    // The open containers: the index position of each opening bracket, and the most levels of a container that
    // closed inside each (inner[0] belongs to the document).
    constexpr u32 kDepth = 128;
    u32 opened[kDepth], inner[kDepth + 1];
    u32 depth = 0;
    inner[0] = 0;
    for (u32 index = 0; index < structurals.count; ++index) {
        const signed char step = kNesting.step[static_cast<u8>(input.data[structurals.data[index]])];
        if (step > 0) {
            if (depth == kDepth) return false;
            opened[depth++] = index;
            inner[depth] = 0;
        } else if (step < 0) {
            if (!depth) return false;
            const u32 levels = inner[depth] + 1;
            ends[opened[--depth]] = index;
            ends[index] = levels;
            inner[depth] = maximum(inner[depth], levels);
        }
    }
    return depth == 0;
}

// An encoded name that starts with a backslash, against ASCII bytes. Out of line, so the comparison of a name without
// escapes saves no registers and builds no string on the stack.
__declspec(noinline) static bool escaped_equals_ascii(const char* encoded, u32 encoded_bytes, const char* ascii,
                                                      u32 ascii_bytes) noexcept {
    const JsonString value{{encoded, encoded_bytes}, 0, false};
    u32 source_offset = 0;
    for (u32 index = 0; index < ascii_bytes; ++index) {
        if (source_offset == encoded_bytes) return false;
        u32 codepoint = static_cast<u8>(encoded[source_offset++]);
        // A multibyte UTF-8 scalar cannot equal an ASCII byte. Only escapes
        // need decoding (including escaped ASCII field names).
        if (codepoint == '\\') {
            --source_offset;
            u32 decoded;
            if (!decode_string_scalar(value, &source_offset, &decoded)) return false;
            codepoint = decoded;
        }
        // Sign-extension puts non-ASCII expected bytes outside Unicode's range,
        // so the equality check also rejects them without a second branch.
        if (codepoint != static_cast<u32>(static_cast<signed char>(ascii[index]))) return false;
    }
    return source_offset == encoded_bytes;
}

bool json_string_equals_ascii_bytes(const char* encoded, u32 encoded_bytes, const char* ascii_data,
                                    u32 ascii_bytes) noexcept {
    if ((!encoded && encoded_bytes != 0) || (!ascii_data && ascii_bytes != 0)) {
        return false;
    }
    // Up to the first backslash each encoded byte is one scalar, so compare
    // bytes directly; only an escape needs the decoding loop.
    const u32 common = minimum(encoded_bytes, ascii_bytes);
    u32 literal = 0;
    while (literal < common && encoded[literal] != '\\') {
        if (encoded[literal] != ascii_data[literal] || static_cast<signed char>(ascii_data[literal]) < 0)
            return false;
        ++literal;
    }
    if (literal == encoded_bytes) return literal == ascii_bytes;
    if (encoded[literal] != '\\') return false;
    // The bytes before the backslash are equal: the rest compares from the backslash on.
    return escaped_equals_ascii(encoded + literal, encoded_bytes - literal, ascii_data + literal, ascii_bytes - literal);
}

bool json_string_equals_utf8(const JsonString& value, const Utf8View& text) noexcept {
    if ((!value.encoded.data && value.encoded.bytes) || (!text.data && text.bytes)) return false;
    u32 source = 0, target = 0;
    while (source < value.encoded.bytes && target < text.bytes) {
        u32 scalar = 0;
        const auto expected = utf8_decode_one(text.data + target, text.bytes - target);
        if (!expected.valid || !decode_string_scalar(value, &source, &scalar) ||
            scalar != expected.codepoint) return false;
        target += expected.width;
    }
    return source == value.encoded.bytes && target == text.bytes;
}

// A string number in which a byte of the last 16 is not a plain digit, or a head byte before them: an escape that
// spells a digit, else a failure. One scalar at a time; rare, so out of line, and the path of plain digits keeps no
// register live across a call.
__declspec(noinline) static bool decimal_escaped(const JsonString& string, u64* output) noexcept {
    const char* const data = string.encoded.data;
    u64 result = 0;
    for (u32 offset = 0; offset < string.encoded.bytes;) {
        u32 codepoint = static_cast<u8>(data[offset]);
        if (codepoint == '\\') {
            if (!decode_string_scalar(string, &offset, &codepoint))
                return false;
        } else {
            ++offset;
        }
        const u32 digit = codepoint - '0';
        if (digit > 9 || __builtin_mul_overflow(result, u64{10}, &result) ||
            __builtin_add_overflow(result, u64{digit}, &result))
            return false;
    }
    *output = result;
    return true;
}
constexpr u64 kPowersOfTen[17]{1,          10,          100,          1000,          10000,          100000,
                               1000000,    10000000,    100000000,    1000000000,    10000000000,    100000000000,
                               1000000000000, 10000000000000, 100000000000000, 1000000000000000,
                               10000000000000000};
// The decimal number that data[0, bytes) (bytes > 0) spells. The bytes before the last 16 go one at a time (a
// snowflake has at most 3 of them); the last 16 or fewer are one SSE step: right-aligned in a register with zeros
// below, checked, then pairs, fours and eights of digits by multiply-adds (the digit reduction of simdjson; Lemire,
// "Number Parsing at a Gigabyte per Second", 2021). In a string (not null), a backslash starts an escape that may
// spell a digit. Any other byte that is not a digit fails, and so does a value past 2^64 - 1. One copy serves
// strings and numbers.
__declspec(noinline) static bool decimal_to_u64(const char* data, u32 bytes, const JsonString* string,
                                                u64* output) noexcept {
    const u32 head = bytes > 16 ? bytes - 16 : 0;
    u64 result = 0;
    for (u32 offset = 0; offset < head; ++offset) {
        const u32 digit = static_cast<u32>(static_cast<u8>(data[offset])) - '0';
        if (digit > 9)
            return string && decimal_escaped(*string, output);
        if (__builtin_mul_overflow(result, u64{10}, &result) || __builtin_add_overflow(result, u64{digit}, &result))
            return false;
    }
    // One 16-byte load from the first digit when it stays in that digit's page, else the 16 bytes that end with the
    // last digit: those lie in the digit's page and the next one, which the text reaches. Either way the read
    // cannot fault. The asm hides the text's size from the compiler, which could otherwise treat it as undefined.
    const u32 tail = bytes - head, below = 16 - tail;
    const char* last = data + head;
    const bool forward = (reinterpret_cast<usize>(last) & 4095) <= 4080;
    const char* block = forward ? last : last + tail - 16;
    __asm__("" : "+r"(block));
    const __m128i lanes = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    // Lane j takes the digit that ends the number in lane 15, or zero (a control byte from 0x80 up) below it.
    const __m128i control =
        _mm_or_si128(_mm_sub_epi8(lanes, _mm_set1_epi8(static_cast<char>(forward ? below : 0))),
                     _mm_cmpgt_epi8(_mm_set1_epi8(static_cast<char>(below)), lanes));
    const __m128i digits = _mm_shuffle_epi8(
        _mm_sub_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(block)), _mm_set1_epi8('0')), control);
    const __m128i nine = _mm_set1_epi8(9);
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_max_epu8(digits, nine), nine)) != 0xFFFF)
        return string && decimal_escaped(*string, output);
    const __m128i pairs = _mm_maddubs_epi16(digits, _mm_set1_epi16(0x010A));
    const __m128i fours = _mm_madd_epi16(pairs, _mm_set1_epi32(0x0001'0064));
    const __m128i eights = _mm_madd_epi16(_mm_packus_epi32(fours, fours), _mm_set1_epi32(0x0001'2710));
    const u64 halves = static_cast<u64>(_mm_cvtsi128_si64(eights));
    const u64 value = (halves & 0xFFFF'FFFFull) * 100'000'000 + (halves >> 32);
    if (__builtin_mul_overflow(result, kPowersOfTen[tail], &result) || __builtin_add_overflow(result, value, &result))
        return false;
    *output = result;
    return true;
}

bool json_string_to_u64(const JsonString& value, u64* output) noexcept {
    if (!value.encoded.data || value.encoded.bytes == 0) {
        return false;
    }
    return decimal_to_u64(value.encoded.data, value.encoded.bytes, &value, output);
}

// The length of the prefix of data[0, bytes) without a backslash and, when `plain`, without a byte from 0x80 up
// (the bytes that stand for themselves). The last bytes take one 32-byte load when it stays in the page of their
// first byte. That page exists, so the read past the text cannot fault. The asm hides the text's size from the
// compiler, which could otherwise treat that read as undefined. Else the last bytes go one at a time.
MAICORD_HASWELL __forceinline u32 unmarked_prefix(const char* data, u32 bytes, bool plain) noexcept {
    const auto marks = [plain](const char* at) MAICORD_HASWELL noexcept {
        const __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(at));
        const __m256i backslashes = _mm256_cmpeq_epi8(block, _mm256_set1_epi8('\\'));
        return static_cast<u32>(_mm256_movemask_epi8(plain ? _mm256_or_si256(block, backslashes) : backslashes));
    };
    u32 offset = 0;
    for (; bytes - offset >= 32; offset += 32)
        if (const u32 found = marks(data + offset))
            return offset + lowest_set_bit(found);
    const u32 rest = bytes - offset;
    const char* block = data + offset;
    if (rest && (reinterpret_cast<usize>(block) & 4095) <= 4064) {
        __asm__("" : "+r"(block));
        return offset + lowest_set_bit(marks(block) | 1u << rest);
    }
    while (offset < bytes && data[offset] != '\\' && !(plain && static_cast<u8>(data[offset]) >= 0x80)) ++offset;
    return offset;
}

// json_string_decode_utf8 for a string whose first `plain` bytes stand for themselves and whose next byte does
// not. Out of line, so the path of a plain string has a small frame.
MAICORD_HASWELL __declspec(noinline) static JsonStringDecodeResult decode_special_string(const JsonString& value,
                                                                                         Slice<char> output,
                                                                                         u32 plain) noexcept {
    // The first pass validates and counts. A byte outside an escape stands for itself, so runs of ASCII and
    // runs of multi-byte sequences add their own length.
    const char* const data = value.encoded.data;
    const u32 bytes = value.encoded.bytes;
    u32 source_offset = plain, required = plain, first_escape = bytes;
    while (source_offset < bytes) {
        while (source_offset < bytes && static_cast<u8>(data[source_offset]) >= 0x80) {
            const u32 width = utf8_sequence_width(data + source_offset, bytes - source_offset);
            if (!width) {
                return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_string};
            }
            source_offset += width;
            required += width;
        }
        if (source_offset < bytes && data[source_offset] == '\\') {
            if (first_escape == bytes) first_escape = source_offset;
            u32 codepoint = 0;
            if (!decode_string_scalar(value, &source_offset, &codepoint)) {
                return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_string};
            }
            required += codepoint < 0x80 ? 1u : codepoint < 0x800 ? 2u : codepoint < 0x10000 ? 3u : 4u;
        }
        const u32 run = unmarked_prefix(data + source_offset, bytes - source_offset, true);
        source_offset += run;
        required += run;
    }
    if (required > output.count) {
        return JsonStringDecodeResult{required, JsonStringDecodeError::output_too_small};
    }

    // The bytes before the first escape are the text. After it only the escapes need a decoder: the runs between
    // them are copied whole.
    copy_bytes(output.data, data, first_escape);
    source_offset = first_escape;
    u32 destination_offset = first_escape;
    while (source_offset < bytes) {
        u32 codepoint = 0;
        decode_string_scalar(value, &source_offset, &codepoint);
        // The first pass sized this exact scalar sequence, so its bytes fit.
        destination_offset += utf8_encode(codepoint, output.data + destination_offset);
        const u32 run = unmarked_prefix(data + source_offset, bytes - source_offset, false);
        copy_bytes(output.data + destination_offset, data + source_offset, run);
        source_offset += run;
        destination_offset += run;
    }
    return JsonStringDecodeResult{required, JsonStringDecodeError::none};
}

MAICORD_HASWELL JsonStringDecodeResult json_string_decode_utf8(const JsonString& value,
                                                               const Slice<char>& output) noexcept {
    if ((!value.encoded.data && value.encoded.bytes != 0) || (!output.data && output.count != 0)) {
        return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_input};
    }
    const u32 plain = unmarked_prefix(value.encoded.data, value.encoded.bytes, true);
    if (plain != value.encoded.bytes) {
        return decode_special_string(value, output, plain);
    }
    if (plain > output.count) {
        return JsonStringDecodeResult{plain, JsonStringDecodeError::output_too_small};
    }
    copy_bytes(output.data, value.encoded.data, plain);
    return JsonStringDecodeResult{plain, JsonStringDecodeError::none};
}

bool json_value_as_string(const JsonValue& value, JsonString* output) noexcept {
    if (value.kind != JsonValueKind::string || value.source.bytes < 2 ||
        value.source.data[0] != '"' || value.source.data[value.source.bytes - 1] != '"') {
        return false;
    }
    output->encoded = Utf8View{value.source.data + 1, value.source.bytes - 2};
    output->source_offset = value.source_offset + 1;
    output->has_escapes = has_backslash(value.source.data + 1, 0, output->encoded.bytes, output->encoded.bytes + 1);
    return true;
}

bool json_value_to_u64(const JsonValue& value, u64* output) noexcept {
    if (value.kind != JsonValueKind::number || value.source.bytes == 0) {
        return false;
    }
    return decimal_to_u64(value.source.data, value.source.bytes, nullptr, output);
}

bool json_value_to_bool(const JsonValue& value, bool* output) noexcept {
    if (value.kind != JsonValueKind::boolean || !value.source.data) {
        return false;
    }
    // "true" or "false", a word compare each.
    const char* data = value.source.data;
    const u32 bytes = value.source.bytes;
    const bool is_true = bytes == 4 && *reinterpret_cast<const u32 __unaligned*>(data) == word('t', 'r', 'u', 'e');
    if (!is_true &&
        !(bytes == 5 && *reinterpret_cast<const u32 __unaligned*>(data) == word('f', 'a', 'l', 's') && data[4] == 'e')) {
        return false;
    }
    *output = is_true;
    return true;
}

} // namespace maicord
