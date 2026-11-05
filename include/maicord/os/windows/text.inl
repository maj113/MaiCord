// The Windows bodies of maicord/os/text.hpp: inline forwarders to the Win32 conversions. Included by text.hpp only.
#include "maicord/core/win32_kernel.hpp"

namespace maicord {

__forceinline u32 utf16_to_utf8(const wchar_t* text, u32 count, char* output, u32 capacity, bool strict) noexcept {
    if (!count)
        return 0;
    const int bytes = WideCharToMultiByte(CP_UTF8, strict ? WC_ERR_INVALID_CHARS : 0, text, static_cast<int>(count),
                                          output, static_cast<int>(capacity), nullptr, nullptr);
    return bytes > 0 ? static_cast<u32>(bytes) : 0;
}

__forceinline u32 utf8_to_utf16(const char* text, u32 bytes, wchar_t* output, u32 capacity, bool strict) noexcept {
    if (!bytes)
        return 0;
    const int units = MultiByteToWideChar(CP_UTF8, strict ? MB_ERR_INVALID_CHARS : 0, text, static_cast<int>(bytes),
                                          output, static_cast<int>(capacity));
    return units > 0 ? static_cast<u32>(units) : 0;
}

} // namespace maicord
