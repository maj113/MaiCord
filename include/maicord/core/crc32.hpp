#pragma once

#include "maicord/core/base.hpp"

#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall RtlComputeCrc32(
    unsigned long initial, const void* bytes, unsigned long count);
#endif

namespace maicord {

// The IEEE CRC-32 (zip, gzip and PNG's polynomial) of `bytes` continued from `crc`; 0 starts a checksum and the result
// of one call is the `crc` of the next. Windows asks ntdll, which picks the implementation for the host CPU.
#if defined(_WIN32)
__forceinline u32 crc32_extend(u32 crc, const void* bytes, u32 count) noexcept {
    return static_cast<u32>(RtlComputeCrc32(crc, bytes, count));
}
#else
namespace crc32_detail {
// The reflected polynomial's byte table, built at compile time.
inline constexpr auto kTable = [] {
    struct Table {
        u32 value[256];
    } table{};
    for (u32 byte = 0; byte < 256; ++byte) {
        u32 crc = byte;
        for (u32 bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        table.value[byte] = crc;
    }
    return table;
}();
} // namespace crc32_detail

inline u32 crc32_extend(u32 crc, const void* bytes, u32 count) noexcept {
    const auto* data = static_cast<const u8*>(bytes);
    u32 state = ~crc;
    for (u32 index = 0; index < count; ++index) {
        state = crc32_detail::kTable.value[(state ^ data[index]) & 0xFFu] ^ (state >> 8);
    }
    return ~state;
}
#endif

} // namespace maicord
