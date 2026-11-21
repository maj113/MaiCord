#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

inline constexpr u64 kHash64Seed = 14'695'981'039'346'656'037ull;
inline constexpr u64 kHash64Prime = 1'099'511'628'211ull;

__forceinline constexpr u64 hash64_mix(u64 hash, u64 value) noexcept {
    return (hash ^ value) * kHash64Prime;
}

// FNV-1a over the bytes. A null source with a nonzero count hashes to 0.
__forceinline u64 hash64_bytes_inline(u64 hash, const void* source, u32 bytes) noexcept {
    if (bytes == 0) {
        return hash;
    }
    if (!source) {
        return 0;
    }
    const auto* input = static_cast<const u8*>(source);
    const u8* const end = input + bytes;
    do {
        hash = hash64_mix(hash, *input);
    } while (++input != end);
    return hash;
}

__declspec(noinline) inline u64 hash64_bytes(u64 hash, const void* source, u32 bytes) noexcept {
    return hash64_bytes_inline(hash, source, bytes);
}

// 32-bit FNV-1a: the checksum of the small persisted state files.
inline u32 hash32_bytes(const u8* data, u32 bytes) noexcept {
    u32 hash = 2166136261u;
    for (u32 index = 0; index < bytes; ++index) {
        hash = (hash ^ data[index]) * 16777619u;
    }
    return hash;
}

// The bytes, then their count. The callee mixes the count in, so a call site holds no multiply of its own.
__declspec(noinline) inline u64 hash64_counted_bytes(u64 hash, const void* source, u32 bytes) noexcept {
    return hash64_mix(hash64_bytes_inline(hash, source, bytes), bytes);
}

// The 128-bit product of a and b folded to 64 bits, with both factors XORed in: a zero factor keeps the other.
__forceinline u64 hash64_fold(u64 a, u64 b) noexcept {
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    return a ^ b ^ static_cast<u64>(product) ^ static_cast<u64>(product >> 64);
}

// The secrets of rapidhash: odd, with balanced bits. Pi's words mixed the low bits of 16-byte keys worse.
inline constexpr u64 kHash64Secret[3]{0x2D358DCCAA6C78A5ull, 0x8BB84B93962EACC9ull, 0x4B33A62ED433D4A3ull};

// The three chains of hash64_words from 49 bytes, out of line: the registers they need are saved only for such a
// key. Returns the joined chains and the 1-48 bytes left.
struct Hash64Lanes {
    u64 hash;
    const u8* input;
    u32 left;
};
__declspec(noinline) Hash64Lanes hash64_word_lanes(u64 hash, const u8* input, u32 left) noexcept;

// Word at a time, for keys that are only compared in memory (never stored): every byte and the length reach the
// result. One multiply folds 16 bytes, as in wyhash and rapidhash (github.com/Nicoshev/rapidhash): a key of 16
// bytes or less takes one multiply, and from 49 bytes three chains take 48 bytes a round. Reads stay inside the
// key: a short key reads words that overlap, a longer one ends with its last 16 bytes. A null source is read only
// for zero bytes. Keys of 0-3 bytes are tested first: like the earlier word hash, they take two branches.
// The inline form is for a hot site with constant sizes; everything else calls hash64_words.
__forceinline u64 hash64_words_inline(u64 hash, const void* source, u32 bytes) noexcept {
    const auto* input = static_cast<const u8*>(source);
    const auto word = [](const u8* at) noexcept {
        u64 value;
        __builtin_memcpy(&value, at, sizeof(value));
        return value;
    };
    u64 first, last = 0;
    if (bytes < 4) {
        first = bytes ? input[0] | static_cast<u64>(input[bytes >> 1]) << 8 | static_cast<u64>(input[bytes - 1]) << 16
                      : 0;
    } else if (bytes < 8) {
        u32 low, high;
        __builtin_memcpy(&low, input, 4);
        __builtin_memcpy(&high, input + bytes - 4, 4);
        first = low | static_cast<u64>(high) << 32;
    } else if (bytes <= 16) {
        first = word(input);
        last = word(input + bytes - 8);
    } else {
        u32 left = bytes;
        if (left > 48) [[unlikely]] {
            const Hash64Lanes lanes = hash64_word_lanes(hash, input, left);
            hash = lanes.hash;
            input = lanes.input;
            left = lanes.left;
        }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        while (left > 16) {
            hash = hash64_fold(word(input) ^ kHash64Secret[0], word(input + 8) ^ hash);
            input += 16;
            left -= 16;
        }
        first = word(input + left - 16);
        last = word(input + left - 8);
    }
    return hash64_fold(first ^ kHash64Secret[1], last ^ bytes ^ hash);
}

// One copy, in src/core/hash.cpp. Of an inline function the linker keeps the copy of any unit: a test unit compiles
// its copy unoptimized.
__declspec(noinline) u64 hash64_words(u64 hash, const void* source, u32 bytes) noexcept;

} // namespace maicord
