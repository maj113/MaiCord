// First-party compiler support for Linux images: the memory functions the compiler emits calls to (the Linux
// counterpart of runtime_x64.asm). Built with -fno-builtin so the bodies are not turned back into calls of
// themselves. Loads precede stores on the small paths, so memcpy and memmove share the overlap-safe code without
// reading beyond the caller's range; AVX1 only, matching the -mavx contract.
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
        void* d = destination;
        const void* s = source;
        usize c = count;
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(c) : : "memory");
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
        void* d = destination;
        usize c = count;
        __asm__ volatile("rep stosb" : "+D"(d), "+c"(c) : "a"(value) : "memory");
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
