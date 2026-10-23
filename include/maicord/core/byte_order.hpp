#pragma once

#include "maicord/core/base.hpp"
#include "maicord/sys/crt.hpp"
#include "maicord/sys/intrin.hpp"

// Declared as <string.h> and <intrin.h> declare them, without parsing either.
extern "C" void* __cdecl memcpy(void* destination, const void* source, unsigned long long count);
extern "C" unsigned short __cdecl _byteswap_ushort(unsigned short value);
extern "C" unsigned long __cdecl _byteswap_ulong(unsigned long value);
#pragma intrinsic(memcpy, _byteswap_ushort, _byteswap_ulong, _byteswap_uint64)

namespace maicord {

// Loads and stores accept unaligned input and touch exactly the named width.
// Fixed-width intrinsics stay visible under /Ob1 so callers need only plain
// loads, stores and swaps, without call setup or the CRT.
inline u16 byte_load_le16(const u8* input) noexcept {
    u16 value; memcpy(&value, input, sizeof(value)); return value;
}
inline u16 byte_load_be16(const u8* input) noexcept {
    u16 value; memcpy(&value, input, sizeof(value)); return _byteswap_ushort(value);
}
u32 byte_load_le24(const u8* input) noexcept;
inline u32 byte_load_le32(const u8* input) noexcept {
    u32 value; memcpy(&value, input, sizeof(value)); return value;
}
inline u64 byte_load_le64(const u8* input) noexcept {
    u64 value; memcpy(&value, input, sizeof(value)); return value;
}
inline u64 byte_load_be64(const u8* input) noexcept {
    u64 value; memcpy(&value, input, sizeof(value)); return _byteswap_uint64(value);
}
inline u32 byte_load_be32(const u8* input) noexcept {
    u32 value; memcpy(&value, input, sizeof(value)); return _byteswap_ulong(value);
}

inline void byte_store_le16(u8* output, u16 value) noexcept {
    memcpy(output, &value, sizeof(value));
}
inline void byte_store_le32(u8* output, u32 value) noexcept {
    memcpy(output, &value, sizeof(value));
}
inline void byte_store_le64(u8* output, u64 value) noexcept {
    memcpy(output, &value, sizeof(value));
}
inline void byte_store_be32(u8* output, u32 value) noexcept {
    value = _byteswap_ulong(value); memcpy(output, &value, sizeof(value));
}

constexpr u32 byte_fourcc_le(char a, char b, char c, char d) noexcept {
    return static_cast<u32>(static_cast<u8>(a)) | static_cast<u32>(static_cast<u8>(b)) << 8u |
           static_cast<u32>(static_cast<u8>(c)) << 16u | static_cast<u32>(static_cast<u8>(d)) << 24u;
}

} // namespace maicord
