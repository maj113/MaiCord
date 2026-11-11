#pragma once

#include "maicord/core/base.hpp"

#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall RtlComputeCrc32(
    unsigned long initial, const void* bytes, unsigned long count);
#endif

namespace maicord {

// The IEEE CRC-32 (zip, gzip and PNG's polynomial) of `bytes` continued from `crc`; 0 starts a checksum and the result
// of one call is the `crc` of the next. Windows asks ntdll, which picks the implementation for the host CPU; Linux
// folds with carry-less multiplication (src/core/linux/crc32.cpp).
#if defined(_WIN32)
__forceinline u32 crc32_extend(u32 crc, const void* bytes, u32 count) noexcept {
    return static_cast<u32>(RtlComputeCrc32(crc, bytes, count));
}
#else
u32 crc32_extend(u32 crc, const void* bytes, u32 count) noexcept;
#endif

} // namespace maicord
