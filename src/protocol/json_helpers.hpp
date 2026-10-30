#pragma once

#include "maicord/core/byte_range.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord::protocol_detail {

inline bool view_valid(const Utf8View& text) noexcept {
    return byte_range_valid(reinterpret_cast<const u8*>(text.data), text.bytes);
}

inline bool thread_archive_duration(u64 minutes) noexcept {
    return minutes == 60 || minutes == 1440 || minutes == 4320 || minutes == 10080;
}

inline bool unicode_blank(u32 scalar) noexcept {
    return scalar <= 32 || scalar == 0x85 || scalar == 0xa0 || scalar == 0x1680 ||
           (scalar >= 0x2000 && scalar <= 0x200a) || scalar == 0x2028 || scalar == 0x2029 || scalar == 0x202f ||
           scalar == 0x205f || scalar == 0x3000;
}

// A nonempty single-line name without control scalars; nonblank also rejects whitespace-only names.
inline bool name_text_valid(Utf8View name, u32 maximum_bytes, u32 maximum_scalars, bool nonblank) noexcept {
    if (!name.bytes || name.bytes > maximum_bytes || !view_valid(name))
        return false;
    const auto valid = utf8_validate(name);
    if (!valid.valid || !valid.codepoints || valid.codepoints > maximum_scalars)
        return false;
    bool content = false;
    for (u32 at = 0; at < name.bytes;) {
        const auto scalar = utf8_decode_one(name.data + at, name.bytes - at);
        if (!scalar.valid || scalar.codepoint < 32 || (scalar.codepoint >= 127 && scalar.codepoint <= 159))
            return false;
        content |= !unicode_blank(scalar.codepoint);
        at += scalar.width;
    }
    return !nonblank || content;
}

// A member name against an ASCII literal.
inline bool name_is(const JsonString& name, const char* ascii) noexcept {
    return json_string_equals_ascii(name, {ascii, c_string_length(ascii)});
}

// A JSON string into output; null stores zero bytes and succeeds when nullable.
// json_string_decode_utf8 already guarantees valid UTF-8.
template <typename Size>
bool decode_text(const JsonValue& value, char* output, u32 capacity, Size* bytes, bool nullable = true) noexcept {
    if (value.kind == JsonValueKind::null_value) {
        *bytes = 0;
        return nullable;
    }
    JsonString encoded{};
    if (!json_value_as_string(value, &encoded))
        return false;
    const auto decoded = json_string_decode_utf8(encoded, {output, capacity});
    if (!decoded.ok())
        return false;
    *bytes = static_cast<Size>(decoded.bytes);
    return true;
}

// A JSON string decoded through a Scratch-byte buffer and cut to capacity at a code point
// boundary: the copied byte count, or 0 when value is not a string or does not fit Scratch.
template <u32 Scratch> u32 copy_text_truncated(const JsonValue& value, char* output, u32 capacity) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return 0;
    char decoded[Scratch];
    const auto result = json_string_decode_utf8(string, {decoded, Scratch});
    return result.ok() ? utf8_copy_truncated(output, capacity, {decoded, result.bytes}) : 0;
}

// Error is an enum with none, input, capacity and json.
template <typename Error> struct ScannedJson {
    JsonIndexedDocument document;
    Error error;
};
template <typename Error> ScannedJson<Error> scan_json(const Utf8View& body, const Slice<u32>& scratch) noexcept {
    if (!view_valid(body) || !body.bytes || !scratch.data || !scratch.count || !utf8_validate(body).valid)
        return {{}, Error::input};
    const auto scanned = json_scan_structurals_avx1(body, scratch);
    if (!scanned.ok())
        return {{}, scanned.error == JsonScanError::output_overflow ? Error::capacity : Error::json};
    return {{body, {scratch.data, scanned.count}}, Error::none};
}

// A JSON body written key by key; ok turns false once any write fails.
struct JsonBody {
    ProtocolWriter writer;
    bool first = true;
    bool ok = true;

    explicit JsonBody(Slice<u8> output) noexcept : writer{output, 0} {}
    __declspec(noinline) void raw(const char* text) noexcept {
        ok = ok && protocol_writer_append_c_string(&writer, text);
    }
    __declspec(noinline) void byte(u8 value) noexcept {
        ok = ok && protocol_writer_append_byte(&writer, value);
    }
    __declspec(noinline) void number(u64 value) noexcept {
        ok = ok && protocol_writer_append_u64(&writer, value);
    }
    __declspec(noinline) void string(const Utf8View& value) noexcept {
        ok = ok && protocol_writer_append_json_string(&writer, value);
    }
    __declspec(noinline) void snowflake(u64 value) noexcept {
        ok = ok && protocol_writer_append_snowflake(&writer, value);
    }
    __declspec(noinline) void key(const char* name) noexcept {
        if (!first)
            byte(',');
        first = false;
        byte('"');
        raw(name);
        raw("\":");
    }
    __declspec(noinline) void begin_object() noexcept {
        byte('{');
        first = true;
    }
    __declspec(noinline) void end_object() noexcept {
        byte('}');
        first = false;
    }
};

struct MemberFinder {
    Utf8View name;
    JsonValue value;
    bool found;
};
inline bool find_first_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& finder = *static_cast<MemberFinder*>(opaque);
    if (!finder.found && json_string_equals_utf8(name, finder.name)) {
        finder.value = value;
        finder.found = true;
    }
    return true;
}
// The first member `name` of an object of the indexed document; an empty value when absent.
__declspec(noinline) inline JsonValue object_member(const JsonIndexedDocument& document, const JsonValue& object,
                                                    Utf8View name, u32 maximum_depth) noexcept {
    MemberFinder finder{name, {}, false};
    if (object.source.bytes && object.kind == JsonValueKind::object)
        json_visit_indexed_object(document, object, maximum_depth, find_first_member, &finder);
    return finder.value;
}

// A string value: borrowed from the document when it has no escapes, else decoded into the arena.
// Empty when it is not a string or the arena is full.
__declspec(noinline) inline Utf8View arena_string(Arena* storage, const JsonValue& value) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return {};
    if (!string.has_escapes)
        return string.encoded;
    const auto size = json_string_decode_utf8(string, {});
    if (size.error != JsonStringDecodeError::output_too_small && size.error != JsonStringDecodeError::none)
        return {};
    char* decoded = arena_allocate<char>(storage, size.bytes ? size.bytes : 1u);
    if (!decoded)
        return {};
    const auto result = json_string_decode_utf8(string, {decoded, size.bytes});
    return result.ok() ? Utf8View{decoded, result.bytes} : Utf8View{};
}
} // namespace maicord::protocol_detail
