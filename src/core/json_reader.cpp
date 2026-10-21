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
    // The range was fully validated before (JsonValue::validated): strings,
    // scalars and nested containers are skipped by the structural index
    // alone. Depth limits and index consistency are still enforced.
    bool trusted;
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

bool validate_string_content(IndexedReader* reader, u32 begin, u32 end, bool* has_escapes) noexcept {
    u32 offset = begin;
    bool escaped = false;
    while (offset < end) {
        if (end - offset >= 16) {
            const u32 special = special_bytes(reader->input.data + offset);
            if (!special) {
                offset += 16;
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

__forceinline bool contains_backslash(const char* data, u32 bytes) noexcept {
    u32 offset = 0;
    for (; bytes - offset >= 16; offset += 16)
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + offset)),
                                             _mm_set1_epi8('\\'))))
            return true;
    for (; offset < bytes; ++offset)
        if (data[offset] == '\\') return true;
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
    if (reader->trusted) {
        if (output) has_escapes = contains_backslash(reader->input.data + content_begin, closing_offset - content_begin);
    } else if (!validate_string_content(reader, content_begin, closing_offset, &has_escapes)) {
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

bool validate_number(const char* data, u32 bytes) noexcept {
    u32 offset = 0;
    if (offset < bytes && data[offset] == '-') {
        ++offset;
    }
    if (offset >= bytes) {
        return false;
    }
    if (data[offset] == '0') {
        ++offset;
        if (offset < bytes && data[offset] >= '0' && data[offset] <= '9') {
            return false;
        }
    } else {
        if (data[offset] < '1' || data[offset] > '9') {
            return false;
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
            return false;
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
            return false;
        }
    }
    return offset == bytes;
}

__forceinline bool is_value_end(char value) noexcept {
    return is_whitespace(value) || value == ',' || value == ']' || value == '}';
}

bool skip_value(IndexedReader* reader, u32 depth, JsonValue* output) noexcept;

// Trusted: jumps over a validated object or array by its structural index.
// Quotes are indexed too, but only brackets change the nesting level.
bool skip_container_trusted(IndexedReader* reader, u32 depth) noexcept {
    const u32 count = reader->structurals.count;
    u32 index = reader->structural_index;
    if (index >= count || reader->structurals[index] != reader->offset)
        return fail(reader, JsonReadError::structural_index_mismatch, reader->offset);
    u32 level = 0;
    for (; index < count; ++index) {
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

bool skip_object(IndexedReader* reader, u32 depth) noexcept {
    if (depth > reader->maximum_depth) {
        return fail(reader, JsonReadError::depth_limit, reader->offset);
    }
    if (!consume_structural(reader, '{', JsonReadError::expected_value)) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->offset < reader->input.bytes && reader->input.data[reader->offset] == '}') {
        return consume_structural(reader, '}', JsonReadError::expected_comma_or_end);
    }
    for (;;) {
        if (!parse_string(reader, nullptr, JsonReadError::expected_member_name)) {
            return false;
        }
        skip_whitespace(reader);
        if (!consume_structural(reader, ':', JsonReadError::expected_colon)) {
            return false;
        }
        if (!skip_value(reader, depth + 1, nullptr)) {
            return false;
        }
        skip_whitespace(reader);
        if (reader->offset < reader->input.bytes && reader->input.data[reader->offset] == '}') {
            return consume_structural(reader, '}', JsonReadError::expected_comma_or_end);
        }
        if (!consume_structural(reader, ',', JsonReadError::expected_comma_or_end)) {
            return false;
        }
        skip_whitespace(reader);
    }
}

bool skip_array(IndexedReader* reader, u32 depth) noexcept {
    if (depth > reader->maximum_depth) {
        return fail(reader, JsonReadError::depth_limit, reader->offset);
    }
    if (!consume_structural(reader, '[', JsonReadError::expected_value)) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->offset < reader->input.bytes && reader->input.data[reader->offset] == ']') {
        return consume_structural(reader, ']', JsonReadError::expected_comma_or_end);
    }
    for (;;) {
        if (!skip_value(reader, depth + 1, nullptr)) {
            return false;
        }
        skip_whitespace(reader);
        if (reader->offset < reader->input.bytes && reader->input.data[reader->offset] == ']') {
            return consume_structural(reader, ']', JsonReadError::expected_comma_or_end);
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
    if (reader->trusted) {
        if (first == '{' || first == '[') {
            kind = first == '{' ? JsonValueKind::object : JsonValueKind::array;
            if (!skip_container_trusted(reader, depth)) return false;
        } else if (first == '"') {
            kind = JsonValueKind::string;
            if (!parse_string(reader, nullptr, JsonReadError::expected_value)) return false;
        } else {
            while (reader->offset < reader->input.bytes && !is_value_end(reader->input.data[reader->offset]))
                ++reader->offset;
            kind = first == 't' || first == 'f' ? JsonValueKind::boolean
                   : first == 'n'               ? JsonValueKind::null_value
                                                : JsonValueKind::number;
        }
    } else if (first == '{') {
        kind = JsonValueKind::object;
        if (!skip_object(reader, depth)) {
            return false;
        }
    } else if (first == '[') {
        kind = JsonValueKind::array;
        if (!skip_array(reader, depth)) {
            return false;
        }
    } else if (first == '"') {
        kind = JsonValueKind::string;
        if (!parse_string(reader, nullptr, JsonReadError::expected_value)) {
            return false;
        }
    } else {
        while (reader->offset < reader->input.bytes && !is_value_end(reader->input.data[reader->offset])) {
            ++reader->offset;
        }
        const u32 bytes = reader->offset - begin;
        if (bytes_equal(reader->input.data + begin, bytes, "true", 4) ||
            bytes_equal(reader->input.data + begin, bytes, "false", 5)) {
            kind = JsonValueKind::boolean;
        } else if (bytes_equal(reader->input.data + begin, bytes, "null", 4)) {
            kind = JsonValueKind::null_value;
        } else if (validate_number(reader->input.data + begin, bytes)) {
            kind = JsonValueKind::number;
        } else {
            const bool number_like = first == '-' || (first >= '0' && first <= '9');
            return fail(reader,
                        number_like ? JsonReadError::invalid_number : JsonReadError::invalid_literal,
                        begin);
        }
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

static JsonReadResult visit_object_range(Utf8View input, ConstSlice<u32> structurals, u32 maximum_depth,
                                         JsonMemberVisitor visitor, void* context, u32 begin,
                                         bool trusted) noexcept {
    if ((!input.data && input.bytes != 0) || (!structurals.data && structurals.count != 0)) {
        return JsonReadResult{0, 0, JsonReadError::invalid_input};
    }

    IndexedReader reader{input, structurals, begin, 0, maximum_depth, 0, JsonReadError::none, trusted};
    if (begin == 0) skip_optional_utf8_bom(&reader);
    skip_whitespace(&reader);
    if (maximum_depth == 0) {
        return JsonReadResult{0, reader.offset, JsonReadError::depth_limit};
    }
    if (!consume_structural(&reader, '{', JsonReadError::expected_root_object)) {
        return JsonReadResult{0, reader.error_offset, reader.error};
    }

    u32 member_count = 0;
    skip_whitespace(&reader);
    if (reader.offset < input.bytes && input.data[reader.offset] != '}') {
        for (;;) {
            JsonString name{};
            if (!parse_string(&reader, &name, JsonReadError::expected_member_name)) {
                break;
            }
            skip_whitespace(&reader);
            if (!consume_structural(&reader, ':', JsonReadError::expected_colon)) {
                break;
            }
            JsonValue value{};
            if (!skip_value(&reader, 2, &value)) {
                break;
            }
            ++member_count;
            if (visitor && !visitor(context, name, value)) {
                fail(&reader, JsonReadError::visitor_rejected, name.source_offset);
                break;
            }
            skip_whitespace(&reader);
            if (reader.offset < input.bytes && input.data[reader.offset] == '}') {
                break;
            }
            if (!consume_structural(&reader, ',', JsonReadError::expected_comma_or_end)) {
                break;
            }
            skip_whitespace(&reader);
        }
    }

    if (reader.error == JsonReadError::none) {
        consume_structural(&reader, '}', JsonReadError::expected_comma_or_end);
    }
    if (reader.error == JsonReadError::none) {
        skip_whitespace(&reader);
        if (reader.offset != input.bytes) {
            fail(&reader, JsonReadError::trailing_data, reader.offset);
        } else if (reader.structural_index != structurals.count) {
            fail(&reader, JsonReadError::structural_index_mismatch, reader.offset);
        }
    }
    return JsonReadResult{member_count, reader.error_offset, reader.error};
}

static JsonReadResult visit_array_range(Utf8View input, ConstSlice<u32> structurals, u32 maximum_depth,
                                        JsonArrayVisitor visitor, void* context, u32 begin,
                                        bool trusted) noexcept {
    if ((!input.data && input.bytes != 0) || (!structurals.data && structurals.count != 0)) {
        return JsonReadResult{0, 0, JsonReadError::invalid_input};
    }

    IndexedReader reader{input, structurals, begin, 0, maximum_depth, 0, JsonReadError::none, trusted};
    if (begin == 0) skip_optional_utf8_bom(&reader);
    skip_whitespace(&reader);
    if (maximum_depth == 0) {
        return JsonReadResult{0, reader.offset, JsonReadError::depth_limit};
    }
    if (!consume_structural(&reader, '[', JsonReadError::expected_root_array)) {
        return JsonReadResult{0, reader.error_offset, reader.error};
    }

    u32 item_count = 0;
    skip_whitespace(&reader);
    if (reader.offset < input.bytes && input.data[reader.offset] != ']') {
        for (;;) {
            JsonValue value{};
            if (!skip_value(&reader, 2, &value)) {
                break;
            }
            if (visitor && !visitor(context, item_count, value)) {
                fail(&reader, JsonReadError::visitor_rejected, value.source_offset);
                break;
            }
            ++item_count;
            skip_whitespace(&reader);
            if (reader.offset < input.bytes && input.data[reader.offset] == ']') {
                break;
            }
            if (!consume_structural(&reader, ',', JsonReadError::expected_comma_or_end)) {
                break;
            }
            skip_whitespace(&reader);
        }
    }

    if (reader.error == JsonReadError::none) {
        consume_structural(&reader, ']', JsonReadError::expected_comma_or_end);
    }
    if (reader.error == JsonReadError::none) {
        skip_whitespace(&reader);
        if (reader.offset != input.bytes) {
            fail(&reader, JsonReadError::trailing_data, reader.offset);
        } else if (reader.structural_index != structurals.count) {
            fail(&reader, JsonReadError::structural_index_mismatch, reader.offset);
        }
    }
    return JsonReadResult{item_count, reader.error_offset, reader.error};
}

JsonReadResult json_visit_root_object(Utf8View input, ConstSlice<u32> structurals, u32 maximum_depth,
                                      JsonMemberVisitor visitor, void* context) noexcept {
    return visit_object_range(input, structurals, maximum_depth, visitor, context, 0, false);
}

JsonReadResult json_visit_root_array(Utf8View input, ConstSlice<u32> structurals, u32 maximum_depth,
                                     JsonArrayVisitor visitor, void* context) noexcept {
    return visit_array_range(input, structurals, maximum_depth, visitor, context, 0, false);
}

JsonReadResult json_visit_value_object(const JsonValue& value, ConstSlice<u32> structurals, u32 maximum_depth,
                                       JsonMemberVisitor visitor, void* context) noexcept {
    return visit_object_range(value.source, structurals, maximum_depth, visitor, context, 0, value.validated);
}

JsonReadResult json_visit_value_array(const JsonValue& value, ConstSlice<u32> structurals, u32 maximum_depth,
                                      JsonArrayVisitor visitor, void* context) noexcept {
    return visit_array_range(value.source, structurals, maximum_depth, visitor, context, 0, value.validated);
}

static u32 structural_lower_bound(ConstSlice<u32> offsets, u32 value) noexcept {
    u32 first = 0, last = offsets.count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (offsets[middle] < value) first = middle + 1;
        else last = middle;
    }
    return first;
}

static bool indexed_subview(const JsonIndexedDocument& document, const JsonValue& value,
                            Utf8View* prefix, ConstSlice<u32>* offsets, u32* begin) noexcept {
    if (!document.input.data || !document.structurals.data || !value.source.data || !value.source.bytes)
        return false;
    const auto root = reinterpret_cast<usize>(document.input.data);
    const auto source = reinterpret_cast<usize>(value.source.data);
    if (source < root || source - root > document.input.bytes) return false;
    *begin = static_cast<u32>(source - root);
    if (value.source.bytes > document.input.bytes - *begin) return false;
    const u32 end = *begin + value.source.bytes;
    const u32 first_index = structural_lower_bound(document.structurals, *begin);
    const u32 end_index = structural_lower_bound(document.structurals, end);
    *prefix = Utf8View{document.input.data, end};
    *offsets = ConstSlice<u32>{document.structurals.data + first_index, end_index - first_index};
    return true;
}

JsonReadResult json_visit_indexed_object(const JsonIndexedDocument& document, const JsonValue& value,
    u32 maximum_depth, JsonMemberVisitor visitor, void* context) noexcept {
    Utf8View prefix{};
    ConstSlice<u32> offsets{};
    u32 begin = 0;
    if (value.kind != JsonValueKind::object || !indexed_subview(document, value, &prefix, &offsets, &begin))
        return {0, 0, JsonReadError::invalid_input};
    return visit_object_range(prefix, offsets, maximum_depth, visitor, context, begin, value.validated);
}

JsonReadResult json_visit_indexed_array(const JsonIndexedDocument& document, const JsonValue& value,
    u32 maximum_depth, JsonArrayVisitor visitor, void* context) noexcept {
    Utf8View prefix{};
    ConstSlice<u32> offsets{};
    u32 begin = 0;
    if (value.kind != JsonValueKind::array || !indexed_subview(document, value, &prefix, &offsets, &begin))
        return {0, 0, JsonReadError::invalid_input};
    return visit_array_range(prefix, offsets, maximum_depth, visitor, context, begin, value.validated);
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

bool json_string_equals_utf8(const JsonString& value, Utf8View text) noexcept {
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

JsonStringDecodeResult json_string_decode_utf8(const JsonString& value, Slice<char> output) noexcept {
    if ((!value.encoded.data && value.encoded.bytes != 0) || (!output.data && output.count != 0)) {
        return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_input};
    }

    u32 source_offset = 0;
    u32 required = 0;
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

    source_offset = 0;
    u32 destination_offset = 0;
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
    output->has_escapes = contains_backslash(output->encoded.data, output->encoded.bytes);
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
    if (value.source.bytes == 4 && value.source.data[0] == 't' && value.source.data[1] == 'r' &&
        value.source.data[2] == 'u' && value.source.data[3] == 'e') {
        *output = true;
        return true;
    }
    if (value.source.bytes == 5 && value.source.data[0] == 'f' && value.source.data[1] == 'a' &&
        value.source.data[2] == 'l' && value.source.data[3] == 's' && value.source.data[4] == 'e') {
        *output = false;
        return true;
    }
    return false;
}

} // namespace maicord
