#pragma once

#include "maicord/core/base.hpp"
#include "maicord/sys/crt.hpp"
#include "maicord/sys/intrin.hpp"

// Declared as <string.h> and <intrin.h> declare them, without parsing either.
extern "C" void* __cdecl memcpy(void* destination, const void* source, unsigned long long count);
extern "C" unsigned short __cdecl _byteswap_ushort(unsigned short value);
extern "C" unsigned long __cdecl _byteswap_ulong(unsigned long value);
#pragma intrinsic(memcpy, _byteswap_ushort, _byteswap_ulong)

namespace maicord {

// Loads and stores accept unaligned input and touch exactly the named width.
// The 16/32-bit big-endian loads are inline so packed table searches see them
// under /Ob1; fixed-size memcpy plus byteswap intrinsics compile to plain
// loads and swaps without the CRT.
u16 byte_load_le16(const u8* input) noexcept;
inline u16 byte_load_be16(const u8* input) noexcept {
    u16 value; memcpy(&value, input, sizeof(value)); return _byteswap_ushort(value);
}
u32 byte_load_le24(const u8* input) noexcept;
u32 byte_load_le32(const u8* input) noexcept;
u64 byte_load_le64(const u8* input) noexcept;
u64 byte_load_be64(const u8* input) noexcept;
inline u32 byte_load_be32(const u8* input) noexcept {
    u32 value; memcpy(&value, input, sizeof(value)); return _byteswap_ulong(value);
}

void byte_store_le16(u8* output, u16 value) noexcept;
void byte_store_le32(u8* output, u32 value) noexcept;
void byte_store_le64(u8* output, u64 value) noexcept;
void byte_store_be32(u8* output, u32 value) noexcept;

constexpr u32 byte_fourcc_le(char a, char b, char c, char d) noexcept {
    return static_cast<u32>(static_cast<u8>(a)) | static_cast<u32>(static_cast<u8>(b)) << 8u |
           static_cast<u32>(static_cast<u8>(c)) << 16u | static_cast<u32>(static_cast<u8>(d)) << 24u;
}

} // namespace maicord
