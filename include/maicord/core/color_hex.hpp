#pragma once

#include "maicord/core/ascii.hpp"

namespace maicord {

// Hex colour text as typed into colour inputs: an optional '#', then 3 or 6
// digits (#RGB, #RRGGBB), or 4 or 8 (#RGBA, #RRGGBBAA) where the input takes
// alpha. Surrounding spaces are the caller's to trim. The result is
// 0xRRGGBBAA; without alpha digits the alpha byte is 0xFF. Rejected text
// leaves *rgba unchanged.
constexpr bool color_hex_parse(const Utf8View& text, bool alpha, u32* rgba) noexcept {
    if (!rgba || (!text.data && text.bytes))
        return false;
    const u32 at = text.bytes && text.data[0] == '#' ? 1 : 0;
    const u32 digits = text.bytes - at;
    if (digits != 3 && digits != 6 && (!alpha || (digits != 4 && digits != 8)))
        return false;
    const bool shorthand = digits <= 4;
    const u32 channels = shorthand ? digits : digits / 2;
    u32 value = 0;
    for (u32 i = 0; i < channels; ++i) {
        const i32 high = ascii_hex_value(text.data[at + (shorthand ? i : i * 2)]);
        const i32 low = shorthand ? high : ascii_hex_value(text.data[at + i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        value = value << 8 | static_cast<u32>(high * 16 + low);
    }
    *rgba = channels == 4 ? value : value << 8 | 0xFFu;
    return true;
}

// "#RRGGBB" in upper case, or "#RRGGBBAA" when alpha is allowed and not
// opaque. Returns the bytes written, or 0 when output is too small.
constexpr u32 color_hex_format(u32 rgba, bool alpha, Slice<char> output) noexcept {
    const bool translucent = alpha && (rgba & 0xFFu) != 0xFFu;
    const u32 digits = translucent ? 8 : 6;
    if (!output.data || output.count < digits + 1)
        return 0;
    const u32 value = translucent ? rgba : rgba >> 8;
    output[0] = '#';
    for (u32 i = 0; i < digits; ++i)
        output[1 + i] = ascii_hex_digit_upper(value >> ((digits - 1 - i) * 4));
    return digits + 1;
}

} // namespace maicord
