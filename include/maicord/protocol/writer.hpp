#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/core/ascii.hpp"

namespace maicord {

struct ProtocolWriter {
    Slice<u8> output;
    u32 used;
};

bool protocol_writer_append_byte(ProtocolWriter* writer, u8 value) noexcept;
bool protocol_writer_append_bytes(ProtocolWriter* writer, const u8* value, u32 count) noexcept;
__forceinline bool protocol_writer_append_bytes(ProtocolWriter* writer,
                                                const ConstSlice<u8>& value) noexcept {
    return protocol_writer_append_bytes(writer, value.data, value.count);
}
bool protocol_writer_append_c_string(ProtocolWriter* writer, const char* value) noexcept;
bool protocol_writer_append_u64(ProtocolWriter* writer, u64 value) noexcept;
// A snowflake in its JSON form: decimal digits in a string.
bool protocol_writer_append_snowflake(ProtocolWriter* writer, u64 id) noexcept;
// A JSON array of snowflake strings.
bool protocol_writer_append_snowflake_array(ProtocolWriter* writer, const u64* ids, u32 count) noexcept;
bool protocol_writer_append_json_string(ProtocolWriter* writer, const Utf8View& value) noexcept;

// RFC 3986 unreserved characters, which percent-encoding leaves as they are.
constexpr bool url_unreserved(u8 value) noexcept {
    return ascii_is_alnum(value) || value == '-' || value == '_' || value == '.' || value == '~';
}
// Appends value with every byte but the url_unreserved ones as %XX.
bool protocol_writer_append_percent_encoded(ProtocolWriter* writer, const Utf8View& value) noexcept;

// Copies a body staged by writer into output once it was written completely
// and fits. Result is a {bytes, error} pair whose error enum has none and
// capacity, such as SoundboardWriteResult.
template <typename Result>
Result protocol_writer_finish(const ProtocolWriter& writer, bool ok, const Slice<u8>& output,
                              const u8* staged) noexcept {
    using Error = decltype(Result{}.error);
    if (!ok || writer.used > output.count) {
        return {0, Error::capacity};
    }
    copy_bytes(output.data, staged, writer.used);
    return {writer.used, Error::none};
}

template <usize Count>
__forceinline bool protocol_writer_append_literal(ProtocolWriter* writer,
                                                  const char (&literal)[Count]) noexcept {
    static_assert(Count > 0);
    // The length-free call is smaller at every site; a literal holds no NUL.
    return protocol_writer_append_c_string(writer, literal);
}

// protocol_write(&writer, parts...) appends each part in order and stops at the first that does not fit. A part is a
// C string or literal (as is), a char (one byte), an unsigned integer (decimal digits), a Utf8View (its bytes), a
// ConstSlice<u8>, or one of the wrappers below. Wrappers refer to their argument: use them within the call.
struct JsonText { const Utf8View& value; };     // a quoted, escaped JSON string
struct SnowflakeText { u64 id; };               // an id as a JSON string of decimal digits
struct PercentText { const Utf8View& value; };  // percent-encoded bytes
struct SnowflakeArray { const u64* ids; u32 count; };  // ["id","id"]
constexpr JsonText json_text(const Utf8View& value) noexcept { return {value}; }
constexpr SnowflakeText snowflake_text(u64 id) noexcept { return {id}; }
constexpr PercentText percent_text(const Utf8View& value) noexcept { return {value}; }
constexpr SnowflakeArray snowflake_array(const u64* ids, u32 count) noexcept { return {ids, count}; }

__forceinline bool protocol_put(ProtocolWriter* writer, const char* text) noexcept {
    return protocol_writer_append_c_string(writer, text);
}
__forceinline bool protocol_put(ProtocolWriter* writer, char byte) noexcept {
    return protocol_writer_append_byte(writer, static_cast<u8>(byte));
}
// Neither a bool nor a signed integer is a part: use json_bool, or an unsigned integer.
bool protocol_put(ProtocolWriter*, bool) = delete;
template <typename Number>
    requires(__is_unsigned(Number) && !__is_same(Number, bool))
__forceinline bool protocol_put(ProtocolWriter* writer, Number number) noexcept {
    return protocol_writer_append_u64(writer, number);
}
__forceinline bool protocol_put(ProtocolWriter* writer, const Utf8View& text) noexcept {
    return protocol_writer_append_bytes(writer, reinterpret_cast<const u8*>(text.data), text.bytes);
}
__forceinline bool protocol_put(ProtocolWriter* writer, const ConstSlice<u8>& bytes) noexcept {
    return protocol_writer_append_bytes(writer, bytes);
}
__forceinline bool protocol_put(ProtocolWriter* writer, const JsonText& text) noexcept {
    return protocol_writer_append_json_string(writer, text.value);
}
__forceinline bool protocol_put(ProtocolWriter* writer, const SnowflakeText& id) noexcept {
    return protocol_writer_append_snowflake(writer, id.id);
}
__forceinline bool protocol_put(ProtocolWriter* writer, const PercentText& text) noexcept {
    return protocol_writer_append_percent_encoded(writer, text.value);
}
__forceinline bool protocol_put(ProtocolWriter* writer, const SnowflakeArray& ids) noexcept {
    return protocol_writer_append_snowflake_array(writer, ids.ids, ids.count);
}
// or_null(present, part) writes the part, or null when it is absent. The part is a value wrapper (not a literal).
template <typename Part> struct OrNull { bool present; Part value; };
template <typename Part> constexpr OrNull<Part> or_null(bool present, Part value) noexcept {
    return {present, value};
}
// An id that is null when zero; a string that is null when empty.
constexpr OrNull<SnowflakeText> nullable_snowflake(u64 id) noexcept { return {id != 0, {id}}; }
constexpr OrNull<JsonText> nullable_json(const Utf8View& value) noexcept { return {value.bytes != 0, {value}}; }
// "true" or "false", as a protocol_write part.
constexpr const char* json_bool(bool value) noexcept { return value ? "true" : "false"; }
template <typename Part> __forceinline bool protocol_put(ProtocolWriter* writer, const OrNull<Part>& part) noexcept {
    return part.present ? protocol_put(writer, part.value) : protocol_writer_append_c_string(writer, "null");
}
template <typename... Parts> __forceinline bool protocol_write(ProtocolWriter* writer, const Parts&... parts) noexcept {
    return (protocol_put(writer, parts) && ...);
}

// A ProtocolWriter that skips every write after the first one that fails (ok), for sequences too branchy for one
// protocol_write expression. Each kind of part goes through one shared out-of-line function, so a call site costs a
// call per part (as protocol_write does) and no guard of its own.
struct GuardedWriter {
    ProtocolWriter writer;
    bool ok = true;

    explicit GuardedWriter(Slice<u8> output) noexcept : writer{output, 0} {}
    explicit GuardedWriter(Slice<char> output) noexcept
        : writer{{reinterpret_cast<u8*>(output.data), output.count}, 0} {}
    // The bytes still free, and the bytes written (0 once a write failed).
    u32 room() const noexcept { return writer.output.count - writer.used; }
    u32 finish() const noexcept { return ok ? writer.used : 0; }
    __declspec(noinline) void put_text(const char* text) noexcept {
        ok = ok && protocol_writer_append_c_string(&writer, text);
    }
    __declspec(noinline) void put_byte(char byte) noexcept {
        ok = ok && protocol_writer_append_byte(&writer, static_cast<u8>(byte));
    }
    __declspec(noinline) void put_number(u64 number) noexcept {
        ok = ok && protocol_writer_append_u64(&writer, number);
    }
    __declspec(noinline) void put_bytes(const Utf8View& text) noexcept { ok = ok && protocol_put(&writer, text); }
    __declspec(noinline) void put_json(const Utf8View& text) noexcept {
        ok = ok && protocol_writer_append_json_string(&writer, text);
    }
    __declspec(noinline) void put_snowflake(u64 id) noexcept {
        ok = ok && protocol_writer_append_snowflake(&writer, id);
    }
    __declspec(noinline) void put_optional_json(bool present, const Utf8View& text) noexcept {
        ok = ok && (present ? protocol_writer_append_json_string(&writer, text)
                            : protocol_writer_append_c_string(&writer, "null"));
    }
    __declspec(noinline) void put_optional_snowflake(bool present, u64 id) noexcept {
        ok = ok && (present ? protocol_writer_append_snowflake(&writer, id)
                            : protocol_writer_append_c_string(&writer, "null"));
    }
    __forceinline void put_one(const char* text) noexcept { put_text(text); }
    __forceinline void put_one(char byte) noexcept { put_byte(byte); }
    void put_one(bool) = delete;
    template <typename Number>
        requires(__is_unsigned(Number) && !__is_same(Number, bool))
    __forceinline void put_one(Number number) noexcept { put_number(number); }
    __forceinline void put_one(const Utf8View& text) noexcept { put_bytes(text); }
    __forceinline void put_one(const JsonText& text) noexcept { put_json(text.value); }
    __forceinline void put_one(const SnowflakeText& id) noexcept { put_snowflake(id.id); }
    __forceinline void put_one(const OrNull<JsonText>& text) noexcept {
        put_optional_json(text.present, text.value.value);
    }
    __forceinline void put_one(const OrNull<SnowflakeText>& id) noexcept {
        put_optional_snowflake(id.present, id.value.id);
    }
    template <typename Part> __forceinline void put_one(const OrNull<Part>& part) noexcept {
        if (part.present)
            put_one(part.value);
        else
            put_text("null");
    }
    // Writes the parts: a C string or literal, a char, an unsigned integer, a Utf8View, json_text, snowflake_text or
    // or_null of those.
    template <typename... Parts> __forceinline void put(const Parts&... parts) noexcept { (put_one(parts), ...); }
};

} // namespace maicord
