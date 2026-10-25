#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

__declspec(noinline) constexpr u32 decimal_digits(u64 value) noexcept {
    u32 digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

// Writes the decimal digits of value (no terminator); returns their count.
template <typename Char> __declspec(noinline) constexpr u32 decimal_write(Char* output, u64 value) noexcept {
    const u32 digits = decimal_digits(value);
    for (u32 index = digits; index != 0; --index) {
        output[index - 1] = static_cast<Char>('0' + value % 10);
        value /= 10;
    }
    return digits;
}

// As decimal_write, with a leading '-' for negative values.
template <typename Char> constexpr u32 decimal_write_signed(Char* output, i64 value) noexcept {
    if (value >= 0)
        return decimal_write(output, static_cast<u64>(value));
    output[0] = static_cast<Char>('-');
    return 1 + decimal_write(output + 1, ~static_cast<u64>(value) + 1);
}

// As decimal_write, with ',' between groups of three digits ("12,345"); at
// most 26 characters.
template <typename Char> constexpr u32 decimal_write_grouped(Char* output, u64 value) noexcept {
    const u32 digits = decimal_digits(value);
    const u32 count = digits + (digits - 1) / 3;
    u32 at = count;
    for (u32 digit = 0; digit < digits; ++digit) {
        if (digit != 0 && digit % 3 == 0)
            output[--at] = static_cast<Char>(',');
        output[--at] = static_cast<Char>('0' + value % 10);
        value /= 10;
    }
    return count;
}

// 1-20 ASCII digits (leading zeros allowed) that fit in a u64.
constexpr bool decimal_parse(Utf8View text, u64* value) noexcept {
    if (text.bytes == 0 || text.bytes > 20)
        return false;
    u64 parsed = 0;
    for (u32 index = 0; index < text.bytes; ++index) {
        const char c = text.data[index];
        if (c < '0' || c > '9')
            return false;
        const u64 digit = static_cast<u64>(c - '0');
        if (parsed > (~u64{0} - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

// As decimal_parse, with any number of leading zeros.
__declspec(noinline) constexpr bool decimal_parse_padded(Utf8View text, u64* value) noexcept {
    u32 zeros = 0;
    while (zeros + 1 < text.bytes && text.data[zeros] == '0')
        ++zeros;
    return decimal_parse({text.data + zeros, text.bytes - zeros}, value);
}
inline bool decimal_parse_padded(ConstSlice<u8> text, u64* value) noexcept {
    return decimal_parse_padded(Utf8View{reinterpret_cast<const char*>(text.data), text.count}, value);
}

// A canonical snowflake: nonzero decimal without leading zeros.
constexpr bool snowflake_parse(Utf8View text, u64* value) noexcept {
    return text.bytes != 0 && text.data[0] != '0' && decimal_parse(text, value);
}

} // namespace maicord
