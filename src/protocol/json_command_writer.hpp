#pragma once

#include "maicord/core/decimal.hpp"
#include "maicord/protocol/gateway_commands.hpp"
#include "json_write.hpp"

namespace maicord::protocol_detail {

// Emits a command twice: without output to measure it, then into the output.
// `at` counts the bytes either way.
struct JsonCommandWriter {
    u8* output;
    u64 at;

    template <usize N> void literal(const char (&value)[N]) noexcept {
        raw(value, static_cast<u32>(N - 1));
    }
    __declspec(noinline) void raw(const char* value, u32 bytes) noexcept {
        if (output)
            copy_bytes(output + at, value, bytes);
        at += bytes;
    }
    void bytes(ConstSlice<u8> value) noexcept {
        raw(reinterpret_cast<const char*>(value.data), value.count);
    }
    __declspec(noinline) void number(u64 value) noexcept {
        if (output)
            at += decimal_write(output + at, value);
        else
            at += decimal_digits(value);
    }
    void id(u64 value) noexcept {
        literal("\"");
        number(value);
        literal("\"");
    }
    void string(const EncodedString& value) noexcept {
        if (output)
            write_json_string(output + at, value.source);
        at += value.bytes;
    }
    void boolean(bool value) noexcept {
        if (value)
            literal("true");
        else
            literal("false");
    }
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
