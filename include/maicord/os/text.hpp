#pragma once

#include "maicord/core/base.hpp"

// Conversion between the UTF-16 wide strings of shared code (paths, Win32-shaped text) and UTF-8 (D16): the
// WideCharToMultiByte / MultiByteToWideChar calls on Windows (inline forwarders, maicord/os/windows/text.inl), the
// same rules in code on Linux (src/os/linux/text.cpp).

namespace maicord {

// `count` UTF-16 code units of `text` as UTF-8 in output; no terminator is written. The bytes written, or 0 when the
// text is empty, does not fit `capacity` or (strict) holds an unpaired surrogate; a lenient conversion writes
// U+FFFD for each unpaired surrogate instead.
u32 utf16_to_utf8(const wchar_t* text, u32 count, char* output, u32 capacity, bool strict) noexcept;

// `bytes` bytes of UTF-8 `text` as UTF-16 in output; no terminator is written. The code units written, or 0 when the
// text is empty, does not fit `capacity` or (strict) is not valid UTF-8; a lenient conversion writes U+FFFD for each
// invalid byte instead.
u32 utf8_to_utf16(const char* text, u32 bytes, wchar_t* output, u32 capacity, bool strict) noexcept;

} // namespace maicord

#if defined(_WIN32)
#include "maicord/os/windows/text.inl"
#endif
