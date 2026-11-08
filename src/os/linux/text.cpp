// maicord/os/text.hpp on Linux: UTF-16 <-> UTF-8 with the rules of WideCharToMultiByte / MultiByteToWideChar for
// CP_UTF8 (strict = WC_ERR_INVALID_CHARS / MB_ERR_INVALID_CHARS).
#include "maicord/os/text.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {

constexpr u32 kReplacement = 0xFFFD;

// Appends one scalar as UTF-8; false when it does not fit.
bool put_utf8(u32 scalar, char* output, u32 capacity, u32* used) noexcept {
    char encoded[4];
    const u32 need = utf8_encode(scalar, encoded);
    if (*used + need > capacity)
        return false;
    copy_bytes(output + *used, encoded, need);
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
        // An invalid sequence is one byte, replaced (or refused when strict).
        const Utf8DecodeResult decoded = utf8_decode_one(text + at, bytes - at);
        if (!decoded.valid && strict)
            return 0;
        if (!put_utf16(decoded.valid ? decoded.codepoint : kReplacement, output, capacity, &used))
            return 0;
        at += decoded.width;
    }
    return used;
}

} // namespace maicord
