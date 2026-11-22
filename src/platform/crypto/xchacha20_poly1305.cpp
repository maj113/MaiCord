#include "maicord/platform/crypto/xchacha20_poly1305.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "aead_internal.hpp"
#include "maicord/sys/simd_haswell.hpp"

namespace maicord {
namespace {

// "expand 32-byte k", the first row of every ChaCha20 state.
inline __m128i sigma() noexcept {
    return _mm_set_epi32(0x6b206574, 0x79622d32, 0x3320646e, 0x61707865);
}

// The ChaCha20 block of `key` and `row` (the counter word, then the nonce words) and the block of the next counter,
// 128 bytes into `output`. With `hchacha`, only the HChaCha20 key of the first block (draft-irtf-cfrg-xchacha-03
// section 2.2): its first and last rows after the twenty rounds, without the input added back, 32 bytes. The state
// is four rows of four words, with the first block in the low 128 bits of each row register and the second block
// in the high 128 bits. One column round works on whole rows, and turning rows 1-3 by one, two and three words
// makes the diagonals columns (the row layout of Bernstein's vector ChaCha, also Goll and Gueron, "Vectorization on
// ChaCha Stream Cipher", ITNG 2014). Arithmetic and memory access are independent of secret values; no lookup
// tables.
MAICORD_HASWELL void blocks(const u8* key, __m128i row, u8* output, bool hchacha) noexcept {
    // Byte orders that turn each 32-bit word left by 16 and by 8 bits, then the counter step of the second block.
    alignas(32) static constexpr u8 constants[96]{
        2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13, 2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13,
        3, 0, 1, 2, 7, 4, 5, 6, 11, 8, 9, 10, 15, 12, 13, 14, 3, 0, 1, 2, 7, 4, 5, 6, 11, 8, 9, 10, 15, 12, 13, 14,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto* table = reinterpret_cast<const __m256i_u*>(constants);
    const __m256i by16 = _mm256_loadu_si256(table), by8 = _mm256_loadu_si256(table + 1);
    const auto* words = reinterpret_cast<const __m128i*>(key);
    const __m256i a0 = _mm256_broadcastsi128_si256(sigma()),
                  b0 = _mm256_broadcastsi128_si256(_mm_loadu_si128(words)),
                  c0 = _mm256_broadcastsi128_si256(_mm_loadu_si128(words + 1)),
                  d0 = _mm256_add_epi32(_mm256_broadcastsi128_si256(row), _mm256_loadu_si256(table + 2));
    __m256i a = a0, b = b0, c = c0, d = d0;
    const auto quarter = [&]() MAICORD_HASWELL __attribute__((always_inline)) noexcept {
        a = _mm256_add_epi32(a, b);
        d = _mm256_shuffle_epi8(_mm256_xor_si256(d, a), by16);
        c = _mm256_add_epi32(c, d);
        b = _mm256_xor_si256(b, c);
        b = _mm256_or_si256(_mm256_slli_epi32(b, 12), _mm256_srli_epi32(b, 20));
        a = _mm256_add_epi32(a, b);
        d = _mm256_shuffle_epi8(_mm256_xor_si256(d, a), by8);
        c = _mm256_add_epi32(c, d);
        b = _mm256_xor_si256(b, c);
        b = _mm256_or_si256(_mm256_slli_epi32(b, 7), _mm256_srli_epi32(b, 25));
    };
    for (u32 round = 0; round != 10; ++round) {
        quarter();
        b = _mm256_shuffle_epi32(b, 0x39);
        c = _mm256_shuffle_epi32(c, 0x4E);
        d = _mm256_shuffle_epi32(d, 0x93);
        quarter();
        b = _mm256_shuffle_epi32(b, 0x93);
        c = _mm256_shuffle_epi32(c, 0x4E);
        d = _mm256_shuffle_epi32(d, 0x39);
    }
    auto* out = reinterpret_cast<__m256i_u*>(output);
    if (hchacha) {
        _mm256_storeu_si256(out, _mm256_permute2x128_si256(a, d, 0x20));
        return;
    }
    a = _mm256_add_epi32(a, a0);
    b = _mm256_add_epi32(b, b0);
    c = _mm256_add_epi32(c, c0);
    d = _mm256_add_epi32(d, d0);
    // x86 stores each word little-endian, the byte order of a ChaCha block.
    _mm256_storeu_si256(out, _mm256_permute2x128_si256(a, b, 0x20));
    _mm256_storeu_si256(out + 1, _mm256_permute2x128_si256(c, d, 0x20));
    _mm256_storeu_si256(out + 2, _mm256_permute2x128_si256(a, b, 0x31));
    _mm256_storeu_si256(out + 3, _mm256_permute2x128_si256(c, d, 0x31));
}

#if defined(_WIN32)
// The in-house ChaCha20-Poly1305. Linux always has OpenSSL by the time a voice key exists (the Gateway's TLS needs
// it), so it carries no copy.
//
// Poly1305 in two 64-bit words and a small third (h2), as in the 64-bit C code of OpenSSL and of Andrew Moon's
// poly1305-donna: four 64x64->128 products for each block. The clamp leaves the low two bits of r1 zero, so
// s1 = r1 + r1/4 folds 2^130 = 5 into one product.
struct Poly1305 {
    u64 r0, r1, s1, h0, h1, h2, pad0, pad1;
};

inline void poly_initialize(Poly1305& poly, const u8* key) noexcept {
    poly.r0 = byte_load_le64(key) & 0x0ffffffc0fffffffull;
    poly.r1 = byte_load_le64(key + 8) & 0x0ffffffc0ffffffcull;
    poly.s1 = poly.r1 + (poly.r1 >> 2);
    poly.h0 = poly.h1 = poly.h2 = 0;
    poly.pad0 = byte_load_le64(key + 16);
    poly.pad1 = byte_load_le64(key + 24);
}

// All AEAD MAC blocks are a full 16 bytes, including the zero-padded final AAD/ciphertext block and length block.
// h stays below 2^131 between blocks: a partial reduction, as OpenSSL's poly1305_blocks. Carries and the reduction
// use fixed arithmetic, not data-dependent branches or tables.
void poly_block(Poly1305& poly, const u8* input) noexcept {
    using u128 = unsigned __int128;
    const u128 sum0 = u128{poly.h0} + byte_load_le64(input);
    const u128 sum1 = u128{poly.h1} + byte_load_le64(input + 8) + static_cast<u64>(sum0 >> 64);
    const u64 h0 = static_cast<u64>(sum0), h1 = static_cast<u64>(sum1);
    u64 h2 = poly.h2 + static_cast<u64>(sum1 >> 64) + 1;
    const u128 d0 = u128{h0} * poly.r0 + u128{h1} * poly.s1;
    u128 d1 = u128{h0} * poly.r1 + u128{h1} * poly.r0 + u128{h2 * poly.s1};
    h2 *= poly.r0;
    d1 += static_cast<u64>(d0 >> 64);
    h2 += static_cast<u64>(d1 >> 64);
    // The bits from 2^130 up fold back times 5: (h2 >> 2) * 5 = (h2 >> 2) + (h2 & ~3).
    const u64 fold = (h2 >> 2) + (h2 & ~u64{3});
    const u128 low = u128{static_cast<u64>(d0)} + fold;
    const u128 middle = u128{static_cast<u64>(d1)} + static_cast<u64>(low >> 64);
    poly.h0 = static_cast<u64>(low);
    poly.h1 = static_cast<u64>(middle);
    poly.h2 = (h2 & 3) + static_cast<u64>(middle >> 64);
}

void poly_padded(Poly1305& poly, ConstSlice<u8> input) noexcept {
    u32 offset = 0;
    while (input.count - offset >= 16) {
        poly_block(poly, input.data + offset);
        offset += 16;
    }
    if (offset != input.count) {
        u8 last[16]{};
        copy_bytes(last, input.data + offset, input.count - offset);
        poly_block(poly, last);
        secure_clear_bytes(last, sizeof(last));
    }
}

inline void poly_finish(Poly1305& poly, u8* tag) noexcept {
    using u128 = unsigned __int128;
    // h + 5 reaches 2^130 exactly when h >= 2^130-5: then h + 5 modulo 2^130 is the remainder.
    const u128 g0 = u128{poly.h0} + 5;
    const u128 g1 = u128{poly.h1} + static_cast<u64>(g0 >> 64);
    const u64 mask = 0 - ((poly.h2 + static_cast<u64>(g1 >> 64)) >> 2);
    const u64 h0 = (poly.h0 & ~mask) | (static_cast<u64>(g0) & mask);
    const u64 h1 = (poly.h1 & ~mask) | (static_cast<u64>(g1) & mask);
    const u128 low = u128{h0} + poly.pad0;
    byte_store_le64(tag, static_cast<u64>(low));
    byte_store_le64(tag + 8, h1 + poly.pad1 + static_cast<u64>(low >> 64));
    secure_clear_bytes(&poly, sizeof(poly));
}

void authenticate(Poly1305& poly, ConstSlice<u8> aad, ConstSlice<u8> ciphertext, u8* tag) noexcept {
    poly_padded(poly, aad);
    poly_padded(poly, ciphertext);
    u8 lengths[16]{};
    // Slice lengths are u32; the high half of each RFC 8439 u64 stays zero.
    byte_store_le32(lengths, aad.count);
    byte_store_le32(lengths + 8, ciphertext.count);
    poly_block(poly, lengths);
    poly_finish(poly, tag);
}

// The derived key, the nonce row and the last pair of keystream blocks.
struct ChaChaStream {
    const u8* key;
    __m128i row;
    u8 block[128];
};

inline void stream_blocks(ChaChaStream& stream, u32 counter) noexcept {
    blocks(stream.key, _mm_blend_epi16(stream.row, _mm_cvtsi32_si128(static_cast<i32>(counter)), 0x03),
           stream.block, false);
}

// The keystream starts with block 1, the second block of the pair that gave the Poly1305 key.
void xor_stream(ChaChaStream& stream, ConstSlice<u8> input, Slice<u8> output) noexcept {
    const u8* key_stream = stream.block + 64;
    u32 counter = 2, available = 64;
    for (u32 offset = 0; offset != input.count;) {
        if (!available) {
            stream_blocks(stream, counter);
            counter += 2;
            key_stream = stream.block;
            available = 128;
        }
        const u32 count = minimum(available, input.count - offset);
        for (u32 i = 0; i != count; ++i)
            output.data[offset + i] = input.data[offset + i] ^ key_stream[i];
        offset += count;
        key_stream += count;
        available -= count;
    }
}

AeadResult chacha_crypt(const u8* subkey, const u8* row, ConstSlice<u8> aad, ConstSlice<u8> input,
                        Slice<u8> output, ConstSlice<u8> tag, bool encrypt) noexcept {
    ChaChaStream stream;
    stream.key = subkey;
    stream.row = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row));
    stream_blocks(stream, 0);
    Poly1305 poly;
    poly_initialize(poly, stream.block);
    if (encrypt) {
        xor_stream(stream, input, output);
        authenticate(poly, aad, {output.data, input.count}, const_cast<u8*>(tag.data));
    } else {
        u8 expected[16];
        authenticate(poly, aad, input, expected);
        u32 difference = 0;
        for (u32 i = 0; i != 16; ++i)
            difference |= expected[i] ^ tag[i];
        secure_clear_bytes(expected, sizeof(expected));
        if (difference) {
            secure_clear_bytes(stream.block, sizeof(stream.block));
            secure_clear_bytes(output.data, input.count);
            return aead_result(AeadError::authentication_failed);
        }
        xor_stream(stream, input, output);
    }
    secure_clear_bytes(stream.block, sizeof(stream.block));
    return aead_result(AeadError::none, input.count);
}

#endif

AeadResult crypt(XChaChaKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                 Slice<u8> output, ConstSlice<u8> tag, bool encrypt) noexcept {
    if (!key.storage.data || !key.storage.locked)
        return aead_result(AeadError::not_initialized);
    // Every caller passes a 24-byte nonce, a 16-byte tag and an output of the input's size, in its own arrays
    // (packet_crypto.cpp, the Linux secure buffer).
    // The storage holds the key, then the HChaCha20 subkey of the key and the first 16 nonce bytes.
    u8* subkey = key.storage.data + 32;
    blocks(key.storage.data, _mm_loadu_si128(reinterpret_cast<const __m128i*>(nonce.data)), subkey, true);
    // The counter word, then the 96-bit IETF nonce: four zero bytes and the last eight nonce bytes.
    u8 row[16]{};
    copy_bytes(row + 8, nonce.data + 16, 8);
#if defined(_WIN32)
    const auto result = chacha_crypt(subkey, row, aad, input, output, tag, encrypt);
#else
    // OpenSSL builds its key schedule per call in its own heap; the subkey here is the only copy of ours.
    const auto result =
        aead_openssl_chacha_crypt(subkey, {row + 4, 12}, aad, input, output, const_cast<u8*>(tag.data), encrypt);
#endif
    secure_clear_bytes(subkey, 32);
    return result;
}
} // namespace

AeadResult xchacha_initialize(XChaChaKey* key, ConstSlice<u8> secret) noexcept {
    if (key->storage.data || key->storage.locked || secret.count != 32 ||
        !byte_range_valid(secret) ||
        byte_ranges_overlap(secret, {reinterpret_cast<const u8*>(key), sizeof(*key)}))
        return aead_result(AeadError::invalid_input);
#if !defined(_WIN32)
    // There is no in-house cipher here: a system without OpenSSL cannot reach a voice key at all.
    if (!aead_openssl_available())
        return aead_result(AeadError::platform_failure);
#endif
    const auto allocated = secure_buffer_initialize(&key->storage, 64);
    if (!allocated.ok())
        return aead_result(AeadError::storage, 0, static_cast<i32>(allocated.native_error));
    copy_bytes(key->storage.data, secret.data, 32);
    key->storage.bytes = key->storage.capacity;
    return aead_result(AeadError::none);
}

void xchacha_release(XChaChaKey* key) noexcept {
    secure_buffer_release(&key->storage);
}

AeadResult xchacha_encrypt(XChaChaKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept {
    return crypt(key, nonce, aad, plaintext, ciphertext, {tag.data, tag.count}, true);
}
AeadResult xchacha_decrypt(XChaChaKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept {
    return crypt(key, nonce, aad, ciphertext, plaintext, tag, false);
}

} // namespace maicord
