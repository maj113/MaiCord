#include "maicord/core/hash.hpp"

namespace maicord {

// The outliner must not split the loops into calls.
[[clang::no_outline]] __declspec(noinline) u64 hash64_words(u64 hash, const void* source, u32 bytes) noexcept {
    return hash64_words_inline(hash, source, bytes);
}

[[clang::no_outline]] __declspec(noinline) Hash64Lanes hash64_word_lanes(u64 hash, const u8* input,
                                                                        u32 left) noexcept {
    const auto word = [](const u8* at) noexcept {
        u64 value;
        __builtin_memcpy(&value, at, sizeof(value));
        return value;
    };
    const u64 k0 = kHash64Secret[0], k1 = kHash64Secret[1], k2 = kHash64Secret[2];
    // Three chains with their own seeds: their multiplies overlap.
    u64 second = hash ^ k1, third = hash ^ k2;
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    do {
        hash = hash64_fold(word(input) ^ k0, word(input + 8) ^ hash);
        second = hash64_fold(word(input + 16) ^ k1, word(input + 24) ^ second);
        third = hash64_fold(word(input + 32) ^ k2, word(input + 40) ^ third);
        // In a unit compiled for size the SLP vectorizer moves two chains to vector registers, and the shuffles cost
        // more than they save (a 64-byte key took 1.65 times as long).
        asm("" : "+r"(second), "+r"(third));
        input += 48;
        left -= 48;
    } while (left > 48);
    return {hash ^ second ^ third, input, left};
}

} // namespace maicord
