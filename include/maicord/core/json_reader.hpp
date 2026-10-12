#pragma once

#include "maicord/core/json_scan.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {

enum class JsonValueKind : u8 {
    object,
    array,
    string,
    number,
    boolean,
    null_value,
};

struct JsonString {
    Utf8View encoded;
    u32 source_offset;
    bool has_escapes;
};

struct JsonValue {
    Utf8View source;
    u32 source_offset;
    JsonValueKind kind;
    // Set only by the reader, after the value's complete source passed RFC
    // 8259/UTF-8 validation. Visits of a validated value skip re-validating
    // its contents; any other value is validated in full.
    bool validated = false;
};

enum class JsonReadError : u8 {
    none,
    invalid_input,
    structural_index_mismatch,
    expected_root_object,
    expected_root_array,
    expected_member_name,
    expected_colon,
    expected_value,
    expected_comma_or_end,
    invalid_string,
    invalid_escape,
    invalid_unicode_escape,
    invalid_utf8,
    invalid_number,
    invalid_literal,
    depth_limit,
    trailing_data,
    visitor_rejected,
};

struct JsonReadResult {
    u32 member_count;
    u32 error_offset;
    JsonReadError error;

    constexpr bool ok() const noexcept {
        return error == JsonReadError::none;
    }
};

enum class JsonStringDecodeError : u8 {
    none,
    invalid_input,
    invalid_string,
    output_too_small,
};

struct JsonStringDecodeResult {
    u32 bytes;
    JsonStringDecodeError error;

    constexpr bool ok() const noexcept {
        return error == JsonStringDecodeError::none;
    }
};

using JsonMemberVisitor = bool (*)(void* context, JsonString name, JsonValue value) noexcept;
using JsonArrayVisitor = bool (*)(void* context, u32 index, JsonValue value) noexcept;

// Validates one complete RFC 8259 object and visits each root member without
// allocating a DOM. The structural index must come from the same input.
JsonReadResult json_visit_root_object(Utf8View input, ConstSlice<u32> structurals, u32 maximum_depth,
                                      JsonMemberVisitor visitor, void* context) noexcept;
JsonReadResult json_visit_root_array(Utf8View input, ConstSlice<u32> structurals, u32 maximum_depth,
                                     JsonArrayVisitor visitor, void* context) noexcept;
// json_visit_root_* over value.source with an index of exactly that source.
JsonReadResult json_visit_value_object(JsonValue value, ConstSlice<u32> structurals, u32 maximum_depth,
                                       JsonMemberVisitor visitor, void* context) noexcept;
JsonReadResult json_visit_value_array(JsonValue value, ConstSlice<u32> structurals, u32 maximum_depth,
                                      JsonArrayVisitor visitor, void* context) noexcept;

// Nested visitors borrow the immutable index of the complete document. They do
// not rescan, copy/rebase offsets, allocate, or overwrite an active parent's index.
struct JsonIndexedDocument {
    Utf8View input;
    ConstSlice<u32> structurals;
};
JsonReadResult json_visit_indexed_object(const JsonIndexedDocument& document, JsonValue value,
    u32 maximum_depth, JsonMemberVisitor visitor, void* context) noexcept;
JsonReadResult json_visit_indexed_array(const JsonIndexedDocument& document, JsonValue value,
    u32 maximum_depth, JsonArrayVisitor visitor, void* context) noexcept;

bool json_string_equals_ascii_bytes(const char* encoded, u32 encoded_bytes, const char* ascii,
                                    u32 ascii_bytes) noexcept;
// Decodes escapes in value (has_escapes is only a hint). The inline forwarder
// passes scalars so field-name chains do not copy both views to the stack.
inline bool json_string_equals_ascii(JsonString value, Utf8View ascii) noexcept {
    return json_string_equals_ascii_bytes(value.encoded.data, value.encoded.bytes, ascii.data, ascii.bytes);
}
// The same comparison for long field-name chains: a name whose first byte
// differs is rejected without a call.
inline bool json_name_is(JsonString name, Utf8View ascii) noexcept {
    if (name.encoded.data && name.encoded.bytes && ascii.bytes && name.encoded.data[0] != ascii.data[0] &&
        name.encoded.data[0] != '\\')
        return false;
    return json_string_equals_ascii_bytes(name.encoded.data, name.encoded.bytes, ascii.data, ascii.bytes);
}
bool json_string_equals_utf8(JsonString value, Utf8View text) noexcept;
bool json_string_to_u64(JsonString value, u64* output) noexcept;
// Reports the complete decoded byte count without touching output on error or
// insufficient capacity. A null, zero-sized output is a valid size query.
JsonStringDecodeResult json_string_decode_utf8(JsonString value, Slice<char> output) noexcept;
bool json_value_as_string(JsonValue value, JsonString* output) noexcept;
bool json_value_to_u64(JsonValue value, u64* output) noexcept;
bool json_value_to_bool(JsonValue value, bool* output) noexcept;

} // namespace maicord
