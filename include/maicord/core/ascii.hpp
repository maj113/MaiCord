#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

constexpr char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

constexpr u8 ascii_lower(u8 value) noexcept {
    return value >= static_cast<u8>('A') && value <= static_cast<u8>('Z')
               ? static_cast<u8>(value + ('a' - 'A'))
               : value;
}

constexpr wchar_t ascii_lower(wchar_t value) noexcept {
    return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
}

constexpr bool ascii_is_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

constexpr bool ascii_is_digit(u8 value) noexcept {
    return value >= static_cast<u8>('0') && value <= static_cast<u8>('9');
}

constexpr bool ascii_is_hex_digit(char value) noexcept {
    return ascii_is_digit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

constexpr bool ascii_is_lower_hex_digit(char value) noexcept {
    return ascii_is_digit(value) || (value >= 'a' && value <= 'f');
}

// Space, tab, CR or LF.
constexpr bool ascii_is_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

// 0-15 for a hexadecimal digit of either case, otherwise -1.
constexpr i32 ascii_hex_value(char value) noexcept {
    return ascii_is_digit(value)            ? value - '0'
           : value >= 'a' && value <= 'f' ? value - 'a' + 10
           : value >= 'A' && value <= 'F' ? value - 'A' + 10
                                          : -1;
}

// The digit for value & 15.
constexpr char ascii_hex_digit(u32 value) noexcept {
    return "0123456789abcdef"[value & 15];
}

constexpr char ascii_hex_digit_upper(u32 value) noexcept {
    return "0123456789ABCDEF"[value & 15];
}

constexpr bool ascii_is_alnum(char value) noexcept {
    return static_cast<u8>(value - '0') < 10 || static_cast<u8>((static_cast<u8>(value) | 0x20) - 'a') < 26;
}

__declspec(noinline) constexpr bool ascii_equal_ignore_case(const Utf8View& left, const Utf8View& right) noexcept {
    if (left.bytes != right.bytes)
        return false;
    for (u32 index = 0; index < left.bytes; ++index)
        if (ascii_lower(left.data[index]) != ascii_lower(right.data[index]))
            return false;
    return true;
}

// True for an empty needle.
constexpr bool ascii_contains_ignore_case(const Utf8View& text, const Utf8View& needle) noexcept {
    for (u32 at = 0; at + needle.bytes <= text.bytes; ++at)
        if (ascii_equal_ignore_case({text.data + at, needle.bytes}, needle))
            return true;
    return false;
}

constexpr bool ascii_graphic_string(const Utf8View& value, u32 maximum_bytes,
                                    bool require_nonempty = false) noexcept {
    if ((!value.data && value.bytes != 0) || (require_nonempty && value.bytes == 0) ||
        value.bytes > maximum_bytes) {
        return false;
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        const u8 byte = static_cast<u8>(value.data[index]);
        if (byte < 0x21u || byte > 0x7Eu) {
            return false;
        }
    }
    return true;
}

inline bool ascii_graphic_string(ConstSlice<u8> value, u32 maximum_bytes, bool require_nonempty = false) noexcept {
    return ascii_graphic_string(Utf8View{reinterpret_cast<const char*>(value.data), value.count}, maximum_bytes,
                                require_nonempty);
}

} // namespace maicord
