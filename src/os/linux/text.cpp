// maicord/os/text.hpp on Linux: UTF-16 <-> UTF-8 with the rules of WideCharToMultiByte / MultiByteToWideChar for
// CP_UTF8 (strict = WC_ERR_INVALID_CHARS / MB_ERR_INVALID_CHARS).
#include "maicord/os/text.hpp"

namespace maicord {
namespace {

constexpr u32 kReplacement = 0xFFFD;

// Appends one scalar as UTF-8; false when it does not fit.
bool put_utf8(u32 scalar, char* output, u32 capacity, u32* used) noexcept {
    const u32 need = scalar < 0x80 ? 1 : scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
    if (*used + need > capacity)
        return false;
    char* at = output + *used;
    switch (need) {
    case 1:
        at[0] = static_cast<char>(scalar);
        break;
    case 2:
        at[0] = static_cast<char>(0xC0 | (scalar >> 6));
        at[1] = static_cast<char>(0x80 | (scalar & 0x3F));
        break;
    case 3:
        at[0] = static_cast<char>(0xE0 | (scalar >> 12));
        at[1] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3F));
        at[2] = static_cast<char>(0x80 | (scalar & 0x3F));
        break;
    default:
        at[0] = static_cast<char>(0xF0 | (scalar >> 18));
        at[1] = static_cast<char>(0x80 | ((scalar >> 12) & 0x3F));
        at[2] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3F));
        at[3] = static_cast<char>(0x80 | (scalar & 0x3F));
        break;
    }
    *used += need;
    return true;
}

bool put_utf16(u32 scalar, wchar_t* output, u32 capacity, u32* used) noexcept {
    if (scalar < 0x10000) {
        if (*used + 1 > capacity)
            return false;
        output[(*used)++] = static_cast<wchar_t>(scalar);
        return true;
    }
    if (*used + 2 > capacity)
        return false;
    scalar -= 0x10000;
    output[(*used)++] = static_cast<wchar_t>(0xD800 | (scalar >> 10));
    output[(*used)++] = static_cast<wchar_t>(0xDC00 | (scalar & 0x3FF));
    return true;
}

// The scalar at text[at] and its width in bytes; width 0 for an invalid sequence (one byte is then skipped).
u32 decode_utf8(const char* text, u32 bytes, u32 at, u32* width) noexcept {
    const u32 lead = static_cast<u8>(text[at]);
    u32 need = 0, scalar = 0, minimum = 0;
    if (lead < 0x80) {
        *width = 1;
        return lead;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        need = 2, scalar = lead & 0x1F, minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        need = 3, scalar = lead & 0x0F, minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        need = 4, scalar = lead & 0x07, minimum = 0x10000;
    } else {
        *width = 0;
        return 0;
    }
    if (at + need > bytes) {
        *width = 0;
        return 0;
    }
    for (u32 index = 1; index < need; ++index) {
        const u32 next = static_cast<u8>(text[at + index]);
        if ((next & 0xC0) != 0x80) {
            *width = 0;
            return 0;
        }
        scalar = (scalar << 6) | (next & 0x3F);
    }
    if (scalar < minimum || scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) {
        *width = 0;
        return 0;
    }
    *width = need;
    return scalar;
}

} // namespace

u32 utf16_to_utf8(const wchar_t* text, u32 count, char* output, u32 capacity, bool strict) noexcept {
    if (!text || !output || count == 0)
        return 0;
    u32 used = 0;
    for (u32 at = 0; at < count; ++at) {
        u32 scalar = static_cast<u16>(text[at]);
        if (scalar >= 0xD800 && scalar <= 0xDBFF && at + 1 < count && static_cast<u16>(text[at + 1]) >= 0xDC00 &&
            static_cast<u16>(text[at + 1]) <= 0xDFFF) {
            scalar = 0x10000 + ((scalar - 0xD800) << 10) + (static_cast<u16>(text[at + 1]) - 0xDC00);
            ++at;
        } else if (scalar >= 0xD800 && scalar <= 0xDFFF) {
            if (strict)
                return 0;
            scalar = kReplacement;
        }
        if (!put_utf8(scalar, output, capacity, &used))
            return 0;
    }
    return used;
}

u32 utf8_to_utf16(const char* text, u32 bytes, wchar_t* output, u32 capacity, bool strict) noexcept {
    if (!text || !output || bytes == 0)
        return 0;
    u32 used = 0;
    for (u32 at = 0; at < bytes;) {
        u32 width = 0;
        u32 scalar = decode_utf8(text, bytes, at, &width);
        if (width == 0) {
            if (strict)
                return 0;
            scalar = kReplacement;
            width = 1;
        }
        if (!put_utf16(scalar, output, capacity, &used))
            return 0;
        at += width;
    }
    return used;
}

} // namespace maicord
