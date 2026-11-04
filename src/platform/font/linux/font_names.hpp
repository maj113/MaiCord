#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/os/ordinal.hpp"
#include "maicord/platform/font/system_font_locator.hpp"

// Helpers shared by the Linux locator's fontconfig and directory-scan sides: family names between UTF-16 (the
// text engine's) and UTF-8 (fontconfig's and the file system's).

namespace maicord::font_locator {

// `units` as NUL-terminated UTF-8 in `output`; the byte count, or 0 when the name is empty, malformed, holds a NUL
// or does not fit.
inline u32 utf16_to_utf8(ConstSlice<u16> units, char* output, u32 capacity) noexcept {
    if (!units.data || units.count == 0 || capacity < 5)
        return 0;
    u32 length = 0;
    for (u32 i = 0; i < units.count; ++i) {
        u32 scalar = units.data[i];
        if (scalar == 0)
            return 0;
        if (scalar >= 0xD800 && scalar < 0xDC00) {
            if (i + 1 >= units.count || units.data[i + 1] < 0xDC00 || units.data[i + 1] >= 0xE000)
                return 0;
            scalar = 0x10000 + ((scalar - 0xD800) << 10) + (units.data[i + 1] - 0xDC00u);
            ++i;
        } else if (scalar >= 0xDC00 && scalar < 0xE000) {
            return 0;
        }
        char encoded[4];
        const u32 width = utf8_encode(scalar, encoded);
        if (width == 0 || length + width >= capacity)
            return 0;
        for (u32 byte = 0; byte < width; ++byte)
            output[length++] = encoded[byte];
    }
    output[length] = 0;
    return length;
}

// The UTF-8 string as a family name; false when it is empty, malformed or longer than a name holds.
inline bool utf8_to_name(const char* text, SystemFontName* output) noexcept {
    if (!text)
        return false;
    u32 bytes = 0;
    while (text[bytes])
        ++bytes;
    u32 count = 0;
    for (u32 offset = 0; offset < bytes;) {
        const Utf8DecodeResult decoded = utf8_decode_one(text + offset, bytes - offset);
        if (!decoded.valid)
            return false;
        offset += decoded.width;
        const u32 units = decoded.codepoint >= 0x10000 ? 2 : 1;
        if (count + units > kSystemFontNameCapacity)
            return false;
        if (units == 2) {
            const u32 value = decoded.codepoint - 0x10000;
            output->units[count++] = static_cast<u16>(0xD800 + (value >> 10));
            output->units[count++] = static_cast<u16>(0xDC00 + (value & 0x3FF));
        } else {
            output->units[count++] = static_cast<u16>(decoded.codepoint);
        }
    }
    output->count = count;
    return count != 0;
}

inline bool names_equal(const SystemFontName& a, ConstSlice<u16> b) noexcept {
    return ordinal_equal(reinterpret_cast<const wchar_t*>(a.units), static_cast<int>(a.count),
                         reinterpret_cast<const wchar_t*>(b.data), static_cast<int>(b.count), true);
}

inline bool names_equal(const SystemFontName& a, const SystemFontName& b) noexcept {
    return names_equal(a, ConstSlice<u16>{b.units, b.count});
}

} // namespace maicord::font_locator
