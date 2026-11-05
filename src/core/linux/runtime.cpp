// First-party compiler support for Linux images: the memory functions the compiler emits calls to (the Linux
// counterpart of runtime_x64.asm). Built with -fno-builtin so the bodies are not turned back into calls of
// themselves. Loads precede stores on the small paths, so memcpy and memmove share the overlap-safe code without
// reading beyond the caller's range; AVX1 only, matching the -mavx contract. Above 128 bytes a forward copy and a
// fill run 4x32-byte AVX loops into a 32-byte aligned destination, and from kStreamingBytes on they stream (non-
// temporal stores): `rep movsb` and `rep stosb` take the fast-string path only for write-back memory and crawl
// (a byte at a time) into the write-combined mappings of GPU buffers (glMapBufferRange), where these loops and the
// streaming stores write whole lines; they are also faster than the string instructions below the last-level cache.
#include "maicord/core/base.hpp"
#include "maicord/sys/crt.hpp"
#include "maicord/sys/crt_extra.hpp"

namespace {

using maicord::u16;
using maicord::u32;
using maicord::u64;
using maicord::u8;
using maicord::usize;

// Unaligned, aliasing access types: the attributes belong to the typedef (as clang's __m128i_u), where they may
// lower the alignment.
typedef char V16 __attribute__((vector_size(16), aligned(1), may_alias));
typedef u64 U64 __attribute__((aligned(1), may_alias));
typedef u32 U32 __attribute__((aligned(1), may_alias));
typedef u16 U16 __attribute__((aligned(1), may_alias));
typedef char V32 __attribute__((vector_size(32), aligned(1), may_alias));
// The streaming stores want the natural alignment, which the copy and fill loops guarantee.
typedef char V32Aligned __attribute__((vector_size(32)));
typedef long long V4Q __attribute__((vector_size(32)));

// Up to 16 bytes: overlapping loads, then overlapping stores.
inline void copy_small(u8* destination, const u8* source, usize count) noexcept {
    if (count >= 8) {
        const u64 head = *reinterpret_cast<const U64*>(source);
        const u64 tail = *reinterpret_cast<const U64*>(source + count - 8);
        *reinterpret_cast<U64*>(destination) = head;
        *reinterpret_cast<U64*>(destination + count - 8) = tail;
    } else if (count >= 4) {
        const u32 head = *reinterpret_cast<const U32*>(source);
        const u32 tail = *reinterpret_cast<const U32*>(source + count - 4);
        *reinterpret_cast<U32*>(destination) = head;
        *reinterpret_cast<U32*>(destination + count - 4) = tail;
    } else if (count >= 2) {
        const u16 head = *reinterpret_cast<const U16*>(source);
        const u16 tail = *reinterpret_cast<const U16*>(source + count - 2);
        *reinterpret_cast<U16*>(destination) = head;
        *reinterpret_cast<U16*>(destination + count - 2) = tail;
    } else if (count == 1) {
        *destination = *source;
    }
}

inline V16 load(const u8* address) noexcept {
    return *reinterpret_cast<const V16*>(address);
}

inline void store(u8* address, V16 value) noexcept {
    *reinterpret_cast<V16*>(address) = value;
}

inline V32 load32(const u8* address) noexcept {
    return *reinterpret_cast<const V32*>(address);
}

inline void store32(u8* address, V32 value) noexcept {
    *reinterpret_cast<V32*>(address) = value;
}

inline void stream32(u8* address, V32 value) noexcept {
    __builtin_nontemporal_store(static_cast<V32Aligned>(value), reinterpret_cast<V32Aligned*>(address));
}

// From this many bytes a copy or fill bypasses the caches: past the last-level cache's reach the write-allocate reads
// of ordinary stores cost half the bandwidth, which the string instructions used to save and streaming stores save too.
constexpr usize kStreamingBytes = 1u << 20;

// More than 128 bytes, the destination not overlapping the source from above (below it, or past its end): the first 32
// and the last 128 bytes are loaded before anything is stored and stored after the loop, which covers the unaligned head
// and the remainder and keeps a destination just below the source correct (the loop never overwrites a byte it has
// still to read: it loads each 128 bytes before storing them, and the destination trails the source).
void copy_forward_wide(u8* destination, const u8* source, usize count) noexcept {
    const V32 head = load32(source);
    const V32 t0 = load32(source + count - 128), t1 = load32(source + count - 96), t2 = load32(source + count - 64),
              t3 = load32(source + count - 32);
    u8* const end = destination + count;
    const usize skip = 32 - (reinterpret_cast<usize>(destination) & 31);
    u8* d = destination + skip;
    const u8* s = source + skip;
    if (count >= kStreamingBytes) {
        for (; d + 128 <= end; d += 128, s += 128) {
            const V32 a = load32(s), b = load32(s + 32), c = load32(s + 64), e = load32(s + 96);
            stream32(d, a);
            stream32(d + 32, b);
            stream32(d + 64, c);
            stream32(d + 96, e);
        }
    } else {
        for (; d + 128 <= end; d += 128, s += 128) {
            const V32 a = load32(s), b = load32(s + 32), c = load32(s + 64), e = load32(s + 96);
            store32(d, a);
            store32(d + 32, b);
            store32(d + 64, c);
            store32(d + 96, e);
        }
    }
    store32(destination, head);
    store32(end - 128, t0);
    store32(end - 96, t1);
    store32(end - 64, t2);
    store32(end - 32, t3);
    if (count >= kStreamingBytes) {
        __builtin_ia32_sfence();
    }
}

// More than 128 bytes of `value` bytes.
void fill_wide(u8* destination, int value, usize count) noexcept {
    const long long pattern = static_cast<long long>(0x0101010101010101ull * static_cast<u8>(value));
    const V4Q wide = {pattern, pattern, pattern, pattern};
    const V32 fill = reinterpret_cast<V32>(wide);
    u8* const end = destination + count;
    u8* d = destination + (32 - (reinterpret_cast<usize>(destination) & 31));
    if (count >= kStreamingBytes) {
        for (; d + 128 <= end; d += 128) {
            stream32(d, fill);
            stream32(d + 32, fill);
            stream32(d + 64, fill);
            stream32(d + 96, fill);
        }
    } else {
        for (; d + 128 <= end; d += 128) {
            store32(d, fill);
            store32(d + 32, fill);
            store32(d + 64, fill);
            store32(d + 96, fill);
        }
    }
    store32(destination, fill);
    store32(end - 128, fill);
    store32(end - 96, fill);
    store32(end - 64, fill);
    store32(end - 32, fill);
    if (count >= kStreamingBytes) {
        __builtin_ia32_sfence();
    }
}

} // namespace

extern "C" {

void* memmove(void* destination_pointer, const void* source_pointer, size_t count) {
    auto* destination = static_cast<u8*>(destination_pointer);
    const auto* source = static_cast<const u8*>(source_pointer);
    if (count <= 16) {
        copy_small(destination, source, count);
    } else if (count <= 32) {
        const V16 head = load(source), tail = load(source + count - 16);
        store(destination, head);
        store(destination + count - 16, tail);
    } else if (count <= 64) {
        const V16 a = load(source), b = load(source + 16), c = load(source + count - 32), d = load(source + count - 16);
        store(destination, a);
        store(destination + 16, b);
        store(destination + count - 32, c);
        store(destination + count - 16, d);
    } else if (count <= 128) {
        const V16 a = load(source), b = load(source + 16), c = load(source + 32), d = load(source + 48);
        const V16 e = load(source + count - 64), f = load(source + count - 48), g = load(source + count - 32),
                  h = load(source + count - 16);
        store(destination, a);
        store(destination + 16, b);
        store(destination + 32, c);
        store(destination + 48, d);
        store(destination + count - 64, e);
        store(destination + count - 48, f);
        store(destination + count - 32, g);
        store(destination + count - 16, h);
    } else if (static_cast<usize>(destination - source) >= count) {
        // The destination is below the source or past its end: a forward copy never overwrites unread bytes.
        copy_forward_wide(destination, source, count);
    } else {
        // The destination overlaps the source from above: copy downward in 16-byte steps, the head last.
        const V16 head = load(source);
        usize remaining = count;
        while (remaining > 16) {
            remaining -= 16;
            store(destination + remaining, load(source + remaining));
        }
        store(destination, head);
    }
    return destination_pointer;
}

void* memcpy(void* destination, const void* source, size_t count) {
    return memmove(destination, source, count);
}

void* memset(void* destination_pointer, int value, size_t count) {
    auto* destination = static_cast<u8*>(destination_pointer);
    const u64 pattern = 0x0101010101010101ull * static_cast<u8>(value);
    if (count <= 16) {
        if (count >= 8) {
            *reinterpret_cast<U64*>(destination) = pattern;
            *reinterpret_cast<U64*>(destination + count - 8) = pattern;
        } else if (count >= 4) {
            *reinterpret_cast<U32*>(destination) = static_cast<u32>(pattern);
            *reinterpret_cast<U32*>(destination + count - 4) = static_cast<u32>(pattern);
        } else if (count >= 2) {
            *reinterpret_cast<U16*>(destination) = static_cast<u16>(pattern);
            *reinterpret_cast<U16*>(destination + count - 2) = static_cast<u16>(pattern);
        } else if (count == 1) {
            *destination = static_cast<u8>(value);
        }
    } else if (count <= 128) {
        const V16 fill = {static_cast<char>(value), static_cast<char>(value), static_cast<char>(value),
                          static_cast<char>(value), static_cast<char>(value), static_cast<char>(value),
                          static_cast<char>(value), static_cast<char>(value), static_cast<char>(value),
                          static_cast<char>(value), static_cast<char>(value), static_cast<char>(value),
                          static_cast<char>(value), static_cast<char>(value), static_cast<char>(value),
                          static_cast<char>(value)};
        usize offset = 0;
        for (; count - offset > 16; offset += 16) {
            store(destination + offset, fill);
        }
        store(destination + count - 16, fill);
    } else {
        fill_wide(destination, value, count);
    }
    return destination_pointer;
}

// LLVM merges chains of equality comparisons into memcmp calls and keeps some of them as calls when optimizing for
// size.
int memcmp(const void* left_pointer, const void* right_pointer, size_t count) {
    const auto* left = static_cast<const u8*>(left_pointer);
    const auto* right = static_cast<const u8*>(right_pointer);
    usize offset = 0;
    for (; count - offset >= 8; offset += 8) {
        const u64 a = *reinterpret_cast<const U64*>(left + offset);
        const u64 b = *reinterpret_cast<const U64*>(right + offset);
        if (a != b) {
            const u32 bit = static_cast<u32>(__builtin_ctzll(a ^ b)) & ~7u;
            return static_cast<int>((a >> bit) & 0xff) - static_cast<int>((b >> bit) & 0xff);
        }
    }
    for (; offset < count; ++offset) {
        if (left[offset] != right[offset]) {
            return static_cast<int>(left[offset]) - static_cast<int>(right[offset]);
        }
    }
    return 0;
}

// The equality-only form LLVM turns a memcmp compared with zero into when it optimizes across the whole program.
int bcmp(const void* left, const void* right, size_t count) {
    return memcmp(left, right, count);
}

} // extern "C"
