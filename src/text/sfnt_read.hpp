#pragma once
#include "maicord/text/base.hpp"

// Bounds checks, big-endian reads and work budgets shared by the table parsers.
namespace maicord::sfnt_read {
inline bool span(ConstSlice<u8> b, u32 at, u32 size) noexcept {
    return b.data && at <= b.count && size <= b.count - at;
}
inline ConstSlice<u8> view(ConstSlice<u8> b, u32 at, u32 size) noexcept { return {b.data + at, size}; }
inline u16 be16(const u8* p) noexcept { return byte_load_be16(p); }
inline i16 s16(const u8* p) noexcept { return static_cast<i16>(byte_load_be16(p)); }
inline u32 be24(const u8* p) noexcept { return (static_cast<u32>(p[0]) << 16u) | byte_load_be16(p + 1); }
inline u32 be32(const u8* p) noexcept { return byte_load_be32(p); }
inline u16 r16(ConstSlice<u8> b, u32 at) noexcept { return byte_load_be16(b.data + at); }
inline u32 r32(ConstSlice<u8> b, u32 at) noexcept { return byte_load_be32(b.data + at); }
// Big-endian unsigned value of one to four bytes.
inline u32 packed(const u8* p, u32 size) noexcept {
    u32 value = 0;
    for (u32 i = 0; i < size; ++i) value = (value << 8u) | p[i];
    return value;
}
inline bool spend(u32& work, u32 n = 1) noexcept {
    if (n > work) return false;
    work -= n;
    return true;
}
inline bool spend(u64& work, u64 n = 1) noexcept {
    if (n > work) return false;
    work -= n;
    return true;
}
// Stores the work consumed in used, when given, on every return path.
template <class T> struct WorkReport {
    T initial;
    const T& left;
    T* used;
    ~WorkReport() noexcept {
        if (used) *used = initial - left;
    }
};
} // namespace maicord::sfnt_read
