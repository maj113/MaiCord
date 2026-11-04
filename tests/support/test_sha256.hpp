#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/memory.hpp"

// An independent SHA-256 for tests that compare a large run of outputs by one digest (the crypto matrices hash every
// ciphertext and tag against a digest an outside tool produced). It shares nothing with the platform crypto under
// test, so a defect there cannot hide in both the output and the comparison, and it runs unchanged on every OS.

namespace maicord {

struct TestSha256 {
    u32 state[8];
    u64 bytes;
    u8 block[64];
    u32 used;
};

inline u32 test_sha256_rotate(u32 value, u32 shift) noexcept {
    return (value >> shift) | (value << (32 - shift));
}

inline void test_sha256_compress(TestSha256* hash, const u8* input) noexcept {
    static constexpr u32 k[64]{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    u32 w[64];
    for (u32 i = 0; i != 16; ++i)
        w[i] = (u32{input[i * 4]} << 24) | (u32{input[i * 4 + 1]} << 16) | (u32{input[i * 4 + 2]} << 8) |
               input[i * 4 + 3];
    for (u32 i = 16; i != 64; ++i) {
        const u32 s0 = test_sha256_rotate(w[i - 15], 7) ^ test_sha256_rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = test_sha256_rotate(w[i - 2], 17) ^ test_sha256_rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = hash->state[0], b = hash->state[1], c = hash->state[2], d = hash->state[3];
    u32 e = hash->state[4], f = hash->state[5], g = hash->state[6], h = hash->state[7];
    for (u32 i = 0; i != 64; ++i) {
        const u32 s1 = test_sha256_rotate(e, 6) ^ test_sha256_rotate(e, 11) ^ test_sha256_rotate(e, 25);
        const u32 choose = (e & f) ^ (~e & g);
        const u32 first = h + s1 + choose + k[i] + w[i];
        const u32 s0 = test_sha256_rotate(a, 2) ^ test_sha256_rotate(a, 13) ^ test_sha256_rotate(a, 22);
        const u32 majority = (a & b) ^ (a & c) ^ (b & c);
        const u32 second = s0 + majority;
        h = g, g = f, f = e, e = d + first, d = c, c = b, b = a, a = first + second;
    }
    hash->state[0] += a, hash->state[1] += b, hash->state[2] += c, hash->state[3] += d;
    hash->state[4] += e, hash->state[5] += f, hash->state[6] += g, hash->state[7] += h;
}

inline void test_sha256_begin(TestSha256* hash) noexcept {
    static constexpr u32 initial[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (u32 i = 0; i != 8; ++i)
        hash->state[i] = initial[i];
    hash->bytes = 0;
    hash->used = 0;
}

inline void test_sha256_update(TestSha256* hash, const void* data, u32 count) noexcept {
    const auto* input = static_cast<const u8*>(data);
    hash->bytes += count;
    while (count) {
        const u32 take = count < 64 - hash->used ? count : 64 - hash->used;
        copy_bytes(hash->block + hash->used, input, take);
        hash->used += take;
        input += take;
        count -= take;
        if (hash->used == 64) {
            test_sha256_compress(hash, hash->block);
            hash->used = 0;
        }
    }
}

inline void test_sha256_finish(TestSha256* hash, u8 (&digest)[32]) noexcept {
    const u64 bits = hash->bytes * 8;
    const u8 one = 0x80;
    test_sha256_update(hash, &one, 1);
    const u8 zero = 0;
    while (hash->used != 56)
        test_sha256_update(hash, &zero, 1);
    u8 length[8];
    for (u32 i = 0; i != 8; ++i)
        length[i] = static_cast<u8>(bits >> (56 - i * 8));
    test_sha256_update(hash, length, 8);
    for (u32 i = 0; i != 8; ++i)
        for (u32 j = 0; j != 4; ++j)
            digest[i * 4 + j] = static_cast<u8>(hash->state[i] >> (24 - j * 8));
}

} // namespace maicord
