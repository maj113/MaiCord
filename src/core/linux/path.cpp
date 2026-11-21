#include "maicord/sys/linux/path.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/os/text.hpp"

namespace maicord {

u32 path_to_utf8(const wchar_t* path, char* output, u32 capacity) noexcept {
    if (!path || !output || capacity == 0) {
        return 0;
    }
    u32 length = 0;
    for (u32 index = 0; path[index]; ++index) {
        u32 codepoint = static_cast<u16>(path[index]);
        if (codepoint >= 0xD800 && codepoint < 0xDC00) {
            const u32 low = static_cast<u16>(path[index + 1]);
            if (low < 0xDC00 || low >= 0xE000) {
                return 0;
            }
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            ++index;
        } else if (codepoint >= 0xDC00 && codepoint < 0xE000) {
            return 0;
        }
        if (codepoint == '\\') {
            codepoint = '/';
        }
        char encoded[4];
        const u32 width = utf8_encode(codepoint, encoded);
        if (width == 0 || width >= capacity - length) {
            return 0;
        }
        for (u32 byte = 0; byte < width; ++byte) {
            output[length++] = encoded[byte];
        }
    }
    output[length] = 0;
    return length;
}

// The strict conversion of os/text.hpp with room for the terminator.
u32 path_from_utf8(const char* text, wchar_t* output, u32 capacity) noexcept {
    if (!text || !output || capacity == 0) {
        return 0;
    }
    u32 bytes = 0;
    while (text[bytes]) {
        ++bytes;
    }
    const u32 length = utf8_to_utf16(text, bytes, output, capacity - 1, true);
    output[length] = 0;
    return length;
}

} // namespace maicord
