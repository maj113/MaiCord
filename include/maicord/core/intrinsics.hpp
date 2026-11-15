#pragma once

#include "maicord/core/base.hpp"

#if defined(_WIN32)
#include "maicord/sys/intrin.hpp"
#else
#include "maicord/sys/linux/syscall.hpp"
#endif

// The bit operations shared code uses, as clang builtins on both systems: each is one instruction.

namespace maicord {

// The index of the lowest set bit of a nonzero mask (bsf). The builtin has no result for zero, so the compiler
// adds no instruction for that case.
__forceinline u32 lowest_set_bit(u32 mask) noexcept {
    return static_cast<u32>(__builtin_ctz(mask));
}

__forceinline u32 lowest_set_bit(u64 mask) noexcept {
    return static_cast<u32>(__builtin_ctzll(mask));
}

__forceinline u32 population_count(u64 value) noexcept {
    return static_cast<u32>(__builtin_popcountll(value));
}

__forceinline u16 byte_swap(u16 value) noexcept {
    return __builtin_bswap16(value);
}

__forceinline u32 byte_swap(u32 value) noexcept {
    return __builtin_bswap32(value);
}

__forceinline u64 byte_swap(u64 value) noexcept {
    return __builtin_bswap64(value);
}

// A nonzero identifier of the calling thread, cheap enough for a hot path: the Windows TEB's thread id, a cached
// gettid elsewhere.
__forceinline u32 current_thread_id() noexcept {
#if defined(_WIN32)
    return __readgsdword(0x48);
#else
    static __thread u32 cached;
    if (!cached) {
        cached = static_cast<u32>(syscall1(kSysGettid, 0));
    }
    return cached;
#endif
}

} // namespace maicord
