#pragma once

#include "maicord/core/decimal.hpp"
#include "maicord/protocol/gateway/commands.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "json_write.hpp"

namespace maicord::protocol_detail {

// Emits a command twice: without output to measure it, then into the output.
// `at` counts the bytes either way; ok turns false for a JSON string that cannot be encoded.
struct JsonCommandWriter {
    u8* output;
    u64 at;
    bool ok = true;

    __declspec(noinline) void raw(const char* value, u32 bytes) noexcept {
        if (output)
            copy_bytes(output + at, value, bytes);
        at += bytes;
    }
    __declspec(noinline) void number(u64 value) noexcept {
        if (output)
            at += decimal_write(output + at, value);
        else
            at += decimal_digits(value);
    }
    __declspec(noinline) void byte(char value) noexcept {
        if (output)
            output[at] = static_cast<u8>(value);
        ++at;
    }
    // A snowflake as a JSON string.
    __declspec(noinline) void id(u64 value) noexcept {
        byte('"');
        number(value);
        byte('"');
    }
    void string(const EncodedString& value) noexcept {
        if (output)
            write_json_string(output + at, value.source);
        at += value.bytes;
    }
    // A JSON string measured as it is written.
    __declspec(noinline) void json(const Utf8View& value) noexcept {
        u32 count = 0;
        if (!ok || !measure_json_string(value, &count)) {
            ok = false;
            return;
        }
        if (output)
            write_json_string(output + at, value);
        at += count;
    }

    // put(parts...) writes each part in order. A part is a string literal, a char, an unsigned integer (decimal
    // digits), a bool ("true" or "false"), a Utf8View or ConstSlice<u8> (its bytes), an EncodedString (a
    // measured JSON string), json_text (a JSON string measured as it is written) or snowflake_text(id).
    template <usize N> __forceinline void put_one(const char (&value)[N]) noexcept {
        raw(value, static_cast<u32>(N - 1));
    }
    __forceinline void put_one(char value) noexcept { byte(value); }
    template <typename Number>
        requires(__is_unsigned(Number) && !__is_same(Number, bool))
    __forceinline void put_one(Number value) noexcept { number(value); }
    template <typename Bool>
        requires(__is_same(Bool, bool))
    __forceinline void put_one(Bool value) noexcept {
        if (value)
            put_one("true");
        else
            put_one("false");
    }
    __forceinline void put_one(const Utf8View& value) noexcept { raw(value.data, value.bytes); }
    __forceinline void put_one(const ConstSlice<u8>& value) noexcept {
        raw(reinterpret_cast<const char*>(value.data), value.count);
    }
    __forceinline void put_one(const EncodedString& value) noexcept { string(value); }
    __forceinline void put_one(const JsonText& value) noexcept { json(value.value); }
    __forceinline void put_one(const SnowflakeText& value) noexcept { id(value.id); }
    template <typename... Parts> __forceinline void put(const Parts&... parts) noexcept { (put_one(parts), ...); }
};

// Runs emit(writer) once to measure the command and once to write it.
template <typename Emit> GatewayCommandWriteResult write_json_command(Slice<u8> output, Emit emit) noexcept {
    if (!output.data && output.count)
        return {0, GatewayCommandError::invalid_input};
    JsonCommandWriter measure{};
    emit(measure);
    if (measure.at > ~u32{0})
        return {0, GatewayCommandError::value_too_large};
    const auto bytes = static_cast<u32>(measure.at);
    if (bytes > output.count)
        return {bytes, GatewayCommandError::output_too_small};
    JsonCommandWriter writer{output.data, 0};
    emit(writer);
    return {bytes, GatewayCommandError::none};
}

} // namespace maicord::protocol_detail
