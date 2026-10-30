#include "maicord/core/json_reader.hpp"
#include "maicord/core/ascii.hpp"

#include "maicord/sys/intrin.hpp"
#include "maicord/sys/simd.hpp"

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
};

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

void skip_optional_utf8_bom(IndexedReader* reader) noexcept {
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

bool validate_string_content(IndexedReader* reader, u32 begin, u32 end, bool* has_escapes) noexcept {
    u32 offset = begin;
    bool escaped = false;
    while (offset < end) {
        // Any 16 bytes inside the input may be loaded; those past the string are masked off.
        if (reader->input.bytes - offset >= 16) {
            const u32 left = end - offset;
            u32 special = special_bytes(reader->input.data + offset);
            if (left < 16) special &= low_bits(left);
            if (!special) {
                offset += left < 16 ? left : 16;
                continue;
            }
            unsigned long first = 0;
            _BitScanForward(&first, special);
            offset += first;
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
        if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' ||
            escape == 'n' || escape == 'r' || escape == 't') {
            continue;
        }
        if (escape != 'u') {
            return fail(reader, JsonReadError::invalid_escape, offset - 1);
        }

        u32 first = 0;
        if (!read_hex_quad(reader->input.data, offset, end, &first)) {
            return fail(reader, JsonReadError::invalid_unicode_escape, offset);
        }
        offset += 4;
        if (first >= 0xD800 && first <= 0xDBFF) {
            if (end - offset < 6 || reader->input.data[offset] != '\\' ||
                reader->input.data[offset + 1] != 'u') {
                return fail(reader, JsonReadError::invalid_unicode_escape, offset);
            }
            u32 second = 0;
            if (!read_hex_quad(reader->input.data, offset + 2, end, &second) || second < 0xDC00 ||
                second > 0xDFFF) {
                return fail(reader, JsonReadError::invalid_unicode_escape, offset + 2);
            }
            offset += 6;
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
            return fail(reader, JsonReadError::invalid_unicode_escape, offset - 4);
        }
    }
    *has_escapes = escaped;
    return true;
}

// Whether data[begin, end) holds a backslash; `readable` bytes of data may be loaded.
__forceinline bool has_backslash(const char* data, u32 begin, u32 end, u32 readable) noexcept {
    for (u32 offset = begin; offset < end; offset += 16) {
        if (readable - offset < 16) {
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
    if (!validate_string_content(reader, content_begin, closing_offset, &has_escapes)) {
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

bool bytes_equal(const char* data, u32 bytes, const char* expected, u32 expected_bytes) noexcept {
    if (bytes != expected_bytes) {
        return false;
    }
    for (u32 index = 0; index < bytes; ++index) {
        if (data[index] != expected[index]) {
            return false;
        }
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


bool skip_composite(IndexedReader* reader, u32 depth, char open) noexcept {
    const char close = open == '{' ? '}' : ']';
    if (depth > reader->maximum_depth) {
        return fail(reader, JsonReadError::depth_limit, reader->offset);
    }
    if (!consume_structural(reader, open, JsonReadError::expected_value)) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->offset < reader->input.bytes && reader->input.data[reader->offset] == close) {
        return consume_structural(reader, close, JsonReadError::expected_comma_or_end);
    }
    for (;;) {
        if (open == '{') {
            if (!parse_string(reader, nullptr, JsonReadError::expected_member_name)) {
                return false;
            }
            skip_whitespace(reader);
            if (!consume_structural(reader, ':', JsonReadError::expected_colon)) {
                return false;
            }
        }
        if (!skip_value(reader, depth + 1, nullptr)) {
            return false;
        }
        skip_whitespace(reader);
        if (reader->offset < reader->input.bytes && reader->input.data[reader->offset] == close) {
            return consume_structural(reader, close, JsonReadError::expected_comma_or_end);
        }
        if (!consume_structural(reader, ',', JsonReadError::expected_comma_or_end)) {
            return false;
        }
        skip_whitespace(reader);
    }
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
    switch (escape) {
    case '"':
    case '\\':
    case '/':
        *codepoint = static_cast<u32>(escape);
        return true;
    case 'b':
        *codepoint = 0x08;
        return true;
    case 'f':
        *codepoint = 0x0C;
        return true;
    case 'n':
        *codepoint = 0x0A;
        return true;
    case 'r':
        *codepoint = 0x0D;
        return true;
    case 't':
        *codepoint = 0x09;
        return true;
    case 'u':
        break;
    default:
        return false;
    }

    u32 first = 0;
    if (!read_hex_quad(value.encoded.data, *offset, value.encoded.bytes, &first)) {
        return false;
    }
    *offset += 4;
    if (first >= 0xD800 && first <= 0xDBFF) {
        if (value.encoded.bytes - *offset < 6 || value.encoded.data[*offset] != '\\' ||
            value.encoded.data[*offset + 1] != 'u') {
            return false;
        }
        u32 second = 0;
        if (!read_hex_quad(value.encoded.data, *offset + 2, value.encoded.bytes, &second) ||
            second < 0xDC00 || second > 0xDFFF) {
            return false;
        }
        *offset += 6;
        *codepoint = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
        return true;
    }
    if (first >= 0xDC00 && first <= 0xDFFF) {
        return false;
    }
    *codepoint = first;
    return true;
}

} // namespace

// The members of a validated object, or the elements of a validated array, found from the structural index
// alone: a member's name is two indexed quotes and a colon, a string value two more, and no byte of a string or
// number is examined. The position stays in locals across visitor calls and reaches the reader on exit, where
// the caller continues at the closing bracket. Stops at the first error, which is in the reader.
template <bool Object, typename Visitor>
static void visit_trusted(IndexedReader* reader, Visitor visitor, void* context, u32* hint, u32 base,
                          u32* members) noexcept {
    constexpr char close = Object ? '}' : ']';
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
        [[maybe_unused]] JsonString name{};
        if constexpr (Object) {
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
            // Branch-free nesting level over the indexed bytes; the deepest level is checked once the
            // container closes, and the exact search runs only for a malformed or too deep one.
            u32 level = 0, peak = 0;
            const u32* at = offsets + index;
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
        if constexpr (Object) {
            ++count;
            if (visitor && !visitor(context, name, value)) return stop(JsonReadError::visitor_rejected, name.source_offset);
        } else {
            if (visitor && !visitor(context, count, value)) return stop(JsonReadError::visitor_rejected, value.source_offset);
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

template <bool Object, typename Visitor>
static JsonReadResult visit_range(Utf8View input, ConstSlice<u32> structurals, u32 maximum_depth, Visitor visitor,
                                  void* context, u32 begin, bool trusted, u32* hint = nullptr,
                                  u32 base = 0) noexcept {
    constexpr char open = Object ? '{' : '[';
    constexpr char close = Object ? '}' : ']';
    if ((!input.data && input.bytes != 0) || (!structurals.data && structurals.count != 0)) {
        return JsonReadResult{0, 0, JsonReadError::invalid_input};
    }

    IndexedReader reader{input, structurals, begin, 0, maximum_depth, 0, JsonReadError::none};
    if (begin == 0) skip_optional_utf8_bom(&reader);
    skip_whitespace(&reader);
    if (maximum_depth == 0) {
        return JsonReadResult{0, reader.offset, JsonReadError::depth_limit};
    }
    if (!consume_structural(&reader, open,
                            Object ? JsonReadError::expected_root_object : JsonReadError::expected_root_array)) {
        return JsonReadResult{0, reader.error_offset, reader.error};
    }

    u32 count = 0;
    skip_whitespace(&reader);
    if (reader.offset < input.bytes && input.data[reader.offset] != close) {
        if (trusted) {
            visit_trusted<Object>(&reader, visitor, context, hint, base, &count);
        } else for (;;) {
            [[maybe_unused]] JsonString name{};
            if constexpr (Object) {
                if (!parse_string(&reader, &name, JsonReadError::expected_member_name)) {
                    break;
                }
                skip_whitespace(&reader);
                if (!consume_structural(&reader, ':', JsonReadError::expected_colon)) {
                    break;
                }
            }
            // A nested visit of this member's value starts its index search here.
            if (hint) *hint = base + reader.structural_index;
            JsonValue value{};
            if (!skip_value(&reader, 2, &value)) {
                break;
            }
            if constexpr (Object) {
                ++count;
                if (visitor && !visitor(context, name, value)) {
                    fail(&reader, JsonReadError::visitor_rejected, name.source_offset);
                    break;
                }
            } else {
                if (visitor && !visitor(context, count, value)) {
                    fail(&reader, JsonReadError::visitor_rejected, value.source_offset);
                    break;
                }
                ++count;
            }
            skip_whitespace(&reader);
            if (reader.offset < input.bytes && input.data[reader.offset] == close) {
                break;
            }
            if (!consume_structural(&reader, ',', JsonReadError::expected_comma_or_end)) {
                break;
            }
            skip_whitespace(&reader);
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
    return JsonReadResult{count, reader.error_offset, reader.error};
}

JsonReadResult json_visit_root_object(const Utf8View& input, const ConstSlice<u32>& structurals,
                                      u32 maximum_depth, JsonMemberVisitor visitor, void* context) noexcept {
    return visit_range<true>(input, structurals, maximum_depth, visitor, context, 0, false);
}

JsonReadResult json_visit_root_array(const Utf8View& input, const ConstSlice<u32>& structurals,
                                     u32 maximum_depth, JsonArrayVisitor visitor, void* context) noexcept {
    return visit_range<false>(input, structurals, maximum_depth, visitor, context, 0, false);
}

JsonReadResult json_visit_value_object(const JsonValue& value, const ConstSlice<u32>& structurals,
                                       u32 maximum_depth, JsonMemberVisitor visitor, void* context) noexcept {
    return visit_range<true>(value.source, structurals, maximum_depth, visitor, context, 0, value.validated);
}

JsonReadResult json_visit_value_array(const JsonValue& value, const ConstSlice<u32>& structurals,
                                      u32 maximum_depth, JsonArrayVisitor visitor, void* context) noexcept {
    return visit_range<false>(value.source, structurals, maximum_depth, visitor, context, 0, value.validated);
}

// The position of the first offset >= value, searched outward from hint: consecutive lookups of a document
// land close together, so a visit of a member's value (the hint) or of a later sibling needs few probes.
static u32 structural_find(ConstSlice<u32> offsets, u32 value, u32 hint) noexcept {
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
static bool indexed_subview(const JsonIndexedDocument& document, const JsonValue& value,
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

JsonReadResult json_visit_indexed_object(const JsonIndexedDocument& document, const JsonValue& value,
    u32 maximum_depth, JsonMemberVisitor visitor, void* context) noexcept {
    Utf8View prefix{};
    ConstSlice<u32> offsets{};
    u32 begin = 0, first = 0;
    if (value.kind != JsonValueKind::object || !indexed_subview(document, value, &prefix, &offsets, &begin, &first))
        return {0, 0, JsonReadError::invalid_input};
    return visit_range<true>(prefix, offsets, maximum_depth, visitor, context, begin, value.validated,
                             &document.hint, first);
}

JsonReadResult json_visit_indexed_array(const JsonIndexedDocument& document, const JsonValue& value,
    u32 maximum_depth, JsonArrayVisitor visitor, void* context) noexcept {
    Utf8View prefix{};
    ConstSlice<u32> offsets{};
    u32 begin = 0, first = 0;
    if (value.kind != JsonValueKind::array || !indexed_subview(document, value, &prefix, &offsets, &begin, &first))
        return {0, 0, JsonReadError::invalid_input};
    return visit_range<false>(prefix, offsets, maximum_depth, visitor, context, begin, value.validated,
                              &document.hint, first);
}

bool json_string_equals_ascii_bytes(const char* encoded, u32 encoded_bytes, const char* ascii_data,
                                    u32 ascii_bytes) noexcept {
    const JsonString value{{encoded, encoded_bytes}, 0, false};
    const Utf8View ascii{ascii_data, ascii_bytes};
    if ((!value.encoded.data && value.encoded.bytes != 0) || (!ascii.data && ascii.bytes != 0)) {
        return false;
    }
    // Up to the first backslash each encoded byte is one scalar, so compare
    // bytes directly; only an escape needs the decoding loop below.
    const u32 common = minimum(value.encoded.bytes, ascii.bytes);
    u32 literal = 0;
    while (literal < common && value.encoded.data[literal] != '\\') {
        if (value.encoded.data[literal] != ascii.data[literal] || static_cast<signed char>(ascii.data[literal]) < 0)
            return false;
        ++literal;
    }
    if (literal == value.encoded.bytes) return literal == ascii.bytes;
    if (value.encoded.data[literal] != '\\') return false;
    u32 source_offset = literal;
    for (u32 index = literal; index < ascii.bytes; ++index) {
        if (source_offset == value.encoded.bytes) return false;
        u32 codepoint = static_cast<u8>(value.encoded.data[source_offset++]);
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
        if (codepoint != static_cast<u32>(static_cast<signed char>(ascii.data[index]))) return false;
    }
    return source_offset == value.encoded.bytes;
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

bool json_string_to_u64(const JsonString& value, u64* output) noexcept {
    if (!output || !value.encoded.data || value.encoded.bytes == 0) {
        return false;
    }
    u32 source_offset = 0;
    u64 result = 0;
    while (source_offset < value.encoded.bytes) {
        u32 codepoint = static_cast<u8>(value.encoded.data[source_offset++]);
        if (codepoint == '\\') {
            --source_offset;
            u32 decoded;
            if (!decode_string_scalar(value, &source_offset, &decoded)) return false;
            codepoint = decoded;
        }
        const u32 digit = codepoint - '0';
        if (digit > 9 || result > ~0ull / 10 || (result == ~0ull / 10 && digit > ~0ull % 10)) {
            return false;
        }
        result = result * 10 + digit;
    }
    *output = result;
    return true;
}

// The length of the prefix whose bytes stand for themselves: below 0x80 and not a backslash.
static u32 plain_prefix(const char* data, u32 bytes) noexcept {
    u32 offset = 0;
    for (; bytes - offset >= 16; offset += 16) {
        const __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + offset));
        const u32 special =
            static_cast<u32>(_mm_movemask_epi8(_mm_or_si128(block, _mm_cmpeq_epi8(block, _mm_set1_epi8('\\')))));
        if (special) {
            unsigned long first = 0;
            _BitScanForward(&first, special);
            return offset + first;
        }
    }
    while (offset < bytes && static_cast<u8>(data[offset]) < 0x80 && data[offset] != '\\') ++offset;
    return offset;
}

JsonStringDecodeResult json_string_decode_utf8(const JsonString& value, const Slice<char>& output_view) noexcept {
    // A value copy: the character stores below would otherwise force reloads of the caller's slice.
    const Slice<char> output = output_view;
    if ((!value.encoded.data && value.encoded.bytes != 0) || (!output.data && output.count != 0)) {
        return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_input};
    }

    const u32 plain = plain_prefix(value.encoded.data, value.encoded.bytes);
    u32 source_offset = plain;
    u32 required = plain;
    while (source_offset < value.encoded.bytes) {
        u32 codepoint = 0;
        if (!decode_string_scalar(value, &source_offset, &codepoint)) {
            return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_string};
        }
        const u32 bytes = codepoint < 0x80 ? 1u : codepoint < 0x800 ? 2u : codepoint < 0x10000 ? 3u : 4u;
        // The scalar decoder validates Unicode, and unescaping cannot grow
        // beyond the u32 encoded input length.
        required += bytes;
    }
    if (required > output.count) {
        return JsonStringDecodeResult{required, JsonStringDecodeError::output_too_small};
    }

    // Valid JSON escapes strictly shrink. Equal lengths therefore identify
    // literal UTF-8, which needs no second scalar decode/encode pass.
    if (required == value.encoded.bytes) {
        copy_bytes(output.data, value.encoded.data, required);
        return JsonStringDecodeResult{required, JsonStringDecodeError::none};
    }

    copy_bytes(output.data, value.encoded.data, plain);
    source_offset = plain;
    u32 destination_offset = plain;
    while (source_offset < value.encoded.bytes) {
        u32 codepoint = 0;
        char encoded[4];
        if (!decode_string_scalar(value, &source_offset, &codepoint)) {
            return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_string};
        }
        const u8 bytes = utf8_encode(codepoint, encoded);
        for (u8 index = 0; index < bytes; ++index) {
            output[destination_offset++] = encoded[index];
        }
    }
    return JsonStringDecodeResult{required, JsonStringDecodeError::none};
}

bool json_value_as_string(const JsonValue& value, JsonString* output) noexcept {
    if (!output || value.kind != JsonValueKind::string || value.source.bytes < 2 ||
        value.source.data[0] != '"' || value.source.data[value.source.bytes - 1] != '"') {
        return false;
    }
    output->encoded = Utf8View{value.source.data + 1, value.source.bytes - 2};
    output->source_offset = value.source_offset + 1;
    output->has_escapes = has_backslash(value.source.data + 1, 0, output->encoded.bytes, output->encoded.bytes + 1);
    return true;
}

bool json_value_to_u64(const JsonValue& value, u64* output) noexcept {
    if (!output || value.kind != JsonValueKind::number || value.source.bytes == 0) {
        return false;
    }
    u64 result = 0;
    for (u32 offset = 0; offset < value.source.bytes; ++offset) {
        const char current = value.source.data[offset];
        if (current < '0' || current > '9') {
            return false;
        }
        const u32 digit = static_cast<u32>(current - '0');
        if (result > ~0ull / 10 || (result == ~0ull / 10 && digit > ~0ull % 10)) {
            return false;
        }
        result = result * 10 + digit;
    }
    *output = result;
    return true;
}

bool json_value_to_bool(const JsonValue& value, bool* output) noexcept {
    if (!output || value.kind != JsonValueKind::boolean || !value.source.data) {
        return false;
    }
    const bool is_true = bytes_equal(value.source.data, value.source.bytes, "true", 4);
    if (!is_true && !bytes_equal(value.source.data, value.source.bytes, "false", 5)) {
        return false;
    }
    *output = is_true;
    return true;
}

} // namespace maicord
