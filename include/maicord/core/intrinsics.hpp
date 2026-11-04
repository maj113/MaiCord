#pragma once

#include "maicord/core/base.hpp"

#if defined(_WIN32)
#include "maicord/sys/intrin.hpp"
#else
#include "maicord/sys/linux/calls.hpp"
#endif

// The compiler intrinsics shared code uses, under portable names: the MSVC ones (declared in maicord/sys/intrin.hpp)
// on Windows, where each wrapper is the same instruction as the intrinsic it replaces, and the clang builtins
// elsewhere.

namespace maicord {

// The index of the lowest set bit of a nonzero mask (bsf).
__forceinline u32 lowest_set_bit(u32 mask) noexcept {
#if defined(_WIN32)
    unsigned long index = 0;
    _BitScanForward(&index, mask);
    return static_cast<u32>(index);
#else
    return static_cast<u32>(__builtin_ctz(mask));
#endif
}

__forceinline u32 lowest_set_bit(u64 mask) noexcept {
#if defined(_WIN32)
    unsigned long index = 0;
    _BitScanForward64(&index, mask);
    return static_cast<u32>(index);
#else
    return static_cast<u32>(__builtin_ctzll(mask));
#endif
}

__forceinline u32 population_count(u64 value) noexcept {
#if defined(_WIN32)
    return static_cast<u32>(__popcnt64(value));
#else
    return static_cast<u32>(__builtin_popcountll(value));
#endif
}

__forceinline u16 byte_swap(u16 value) noexcept {
#if defined(_WIN32)
    return _byteswap_ushort(value);
#else
    return __builtin_bswap16(value);
#endif
}

__forceinline u32 byte_swap(u32 value) noexcept {
#if defined(_WIN32)
    return _byteswap_ulong(value);
#else
    return __builtin_bswap32(value);
#endif
}

__forceinline u64 byte_swap(u64 value) noexcept {
#if defined(_WIN32)
    return _byteswap_uint64(value);
#else
    return __builtin_bswap64(value);
#endif
}

// A nonzero identifier of the calling thread, cheap enough for a hot path: the Windows TEB's thread id, a cached
// gettid elsewhere.
__forceinline u32 current_thread_id() noexcept {
#if defined(_WIN32)
    return __readgsdword(0x48);
#else
    static __thread u32 cached;
    if (!cached) {
        cached = static_cast<u32>(sys_gettid());
    }
    return cached;
#endif
}

} // namespace maicord
