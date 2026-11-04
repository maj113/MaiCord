#pragma once

#include "maicord/os/linux/interlocked.hpp"

// The MSVC compiler intrinsics shared code calls by name (declared for Windows in maicord/sys/intrin.hpp), on
// clang's builtins: the same instructions. The parameter types keep the Windows ones where a width matters: the
// 32-bit scans take a 32-bit mask (unsigned long is 64 bits wide here), and their index is an unsigned long, which
// is what the callers declare.

inline unsigned char _BitScanForward(unsigned long* index, unsigned int mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    *index = static_cast<unsigned long>(__builtin_ctz(mask));
    return 1;
}
inline unsigned char _BitScanForward64(unsigned long* index, unsigned long long mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    *index = static_cast<unsigned long>(__builtin_ctzll(mask));
    return 1;
}
inline unsigned char _BitScanReverse(unsigned long* index, unsigned int mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    *index = static_cast<unsigned long>(31 - __builtin_clz(mask));
    return 1;
}
inline unsigned char _BitScanReverse64(unsigned long* index, unsigned long long mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    *index = static_cast<unsigned long>(63 - __builtin_clzll(mask));
    return 1;
}
inline unsigned long long __popcnt64(unsigned long long value) noexcept {
    return static_cast<unsigned long long>(__builtin_popcountll(value));
}

inline unsigned long long _byteswap_uint64(unsigned long long value) noexcept {
    return __builtin_bswap64(value);
}
inline unsigned int _byteswap_ulong(unsigned int value) noexcept {
    return __builtin_bswap32(value);
}
inline unsigned short _byteswap_ushort(unsigned short value) noexcept {
    return __builtin_bswap16(value);
}

// _ReadWriteBarrier and the _Interlocked* family are clang builtins under -fms-extensions, with the Windows
// signatures (the 32-bit `long` of Windows is an int here): declared, not defined.
extern "C" {
void _ReadWriteBarrier(void);
LONG _InterlockedCompareExchange(LONG volatile* destination, LONG exchange, LONG comparand);
LONG _InterlockedExchange(LONG volatile* target, LONG value);
LONG _InterlockedExchangeAdd(LONG volatile* addend, LONG value);
LONG _InterlockedIncrement(LONG volatile* addend);
LONG _InterlockedDecrement(LONG volatile* addend);
LONG _InterlockedOr(LONG volatile* destination, LONG mask);
}
