#pragma once

#include "maicord/core/base.hpp"

// Paths at the system-call boundary. MaiCord's shared code holds paths as UTF-16 wchar_t strings (the Linux build
// compiles with -fshort-wchar, so wchar_t is 16 bits wide as on Windows) and writes either separator; the kernel
// takes UTF-8 and '/'. These convert between the two until the shared code stops naming paths in wchar_t.

namespace maicord {

// Writes `path` as a NUL-terminated UTF-8 string, every '\' as '/'. Returns the byte count without the NUL, or 0
// when the path is empty, holds an unpaired surrogate or does not fit in `capacity` with its terminator.
u32 path_to_utf8(const wchar_t* path, char* output, u32 capacity) noexcept;

// The inverse: `text` as a NUL-terminated UTF-16 string. Returns the character count, or 0 when `text` is empty,
// not valid UTF-8 or does not fit.
u32 path_from_utf8(const char* text, wchar_t* output, u32 capacity) noexcept;

} // namespace maicord
