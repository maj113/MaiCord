#include "maicord/platform/crypto/aes_gcm.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "aead_internal.hpp"

#include "maicord/core/intrinsics.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {
// GHASH. Blocks are byte-reversed ("Z form"), so bit 127-i holds the coefficient
// of x^i. Powers of H are kept twisted (H^k * x^-1 mod P) next to their Karatsuba
// middle operand, so the unshifted 255-bit carryless product of a block and a
// twisted power already has x^f at bit 255-f, and one reduction serves any number
// of blocks. Hashed data is AAD or ciphertext, both public, so partial-block
// copies are not wiped.
struct Sum {
    __m128i low, high, middle;
};
inline __m128i swap_bytes(__m128i value) noexcept {
    return _mm_shuffle_epi8(value, _mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0));
}
inline __m128i xor3(__m128i a, __m128i b, __m128i c) noexcept {
    return _mm_xor_si128(_mm_xor_si128(a, b), c);
}
// Multiply by x^-1: shift left one bit; the bit shifted out is x^0 * x^-1 =
// x^127 + x^6 + x + 1.
inline __m128i twist(__m128i z) noexcept {
    const auto carry = _mm_srai_epi32(_mm_shuffle_epi32(z, 0xff), 31);
    const auto shifted = _mm_or_si128(_mm_slli_epi64(z, 1), _mm_slli_si128(_mm_srli_epi64(z, 63), 8));
    return _mm_xor_si128(shifted, _mm_and_si128(carry, _mm_set_epi32(static_cast<int>(0xc2000000u), 0, 0, 1)));
}
inline void store_power(u8* destination, __m128i value) noexcept {
    const auto twisted = twist(value);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination), twisted);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + 16),
                     _mm_xor_si128(twisted, _mm_srli_si128(twisted, 8)));
}
// Three carryless products per block: low, high and (low^high) * (low^high).
inline void accumulate(Sum& sum, __m128i block, const u8* power) noexcept {
    const auto twisted = _mm_loadu_si128(reinterpret_cast<const __m128i*>(power));
    const auto middle = _mm_loadu_si128(reinterpret_cast<const __m128i*>(power + 16));
    sum.low = _mm_xor_si128(sum.low, _mm_clmulepi64_si128(block, twisted, 0x00));
    sum.high = _mm_xor_si128(sum.high, _mm_clmulepi64_si128(block, twisted, 0x11));
    sum.middle = _mm_xor_si128(
        sum.middle, _mm_clmulepi64_si128(_mm_xor_si128(block, _mm_srli_si128(block, 8)), middle, 0x00));
}
// v ^ v>>1 ^ v>>2 ^ v>>7 as 128-bit shifts. x^128 = x^7+x^2+x+1, and a
// coefficient of x^(128+m) sits at bit 127-m of the low half.
inline __m128i fold(__m128i v) noexcept {
    const auto right = xor3(_mm_srli_epi64(v, 1), _mm_srli_epi64(v, 2), _mm_srli_epi64(v, 7));
    const auto crossing = xor3(_mm_slli_epi64(v, 63), _mm_slli_epi64(v, 62), _mm_slli_epi64(v, 57));
    return xor3(v, right, _mm_srli_si128(crossing, 8));
}
inline __m128i reduce(Sum sum) noexcept {
    const auto cross = xor3(sum.middle, sum.low, sum.high);
    const auto low = _mm_xor_si128(sum.low, _mm_slli_si128(cross, 8));
    const auto high = _mm_xor_si128(sum.high, _mm_srli_si128(cross, 8));
    // The fold pushes at most seven bits below x^0 of the low half, where they
    // are again x^128 and above: fold those once more.
    const auto overflow = _mm_slli_si128(
        xor3(_mm_slli_epi64(low, 63), _mm_slli_epi64(low, 62), _mm_slli_epi64(low, 57)), 8);
    return xor3(high, fold(low), fold(overflow));
}
// ((Y xor X0)*H^n xor X1*H^(n-1) ... xor X(n-1)*H) mod P for blocks of the
// data and, when present, one last 16-byte block; n is at most 8.
inline __m128i hash_blocks(__m128i state, const u8* powers, const u8* data, u32 full,
                           const u8* partial) noexcept {
    const u32 count = full + (partial ? 1u : 0u);
    Sum sum{_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128()};
    auto carried = state;
    for (u32 i = 0; i != count; ++i) {
        const auto block =
            swap_bytes(_mm_loadu_si128(reinterpret_cast<const __m128i*>(i < full ? data + i * 16 : partial)));
        accumulate(sum, _mm_xor_si128(block, carried), powers + (count - 1 - i) * 32);
        carried = _mm_setzero_si128();
    }
    return reduce(sum);
}
inline __m128i hash_part(__m128i state, const u8* powers, const u8* data, u32 bytes) noexcept {
    for (; bytes >= 128; data += 128, bytes -= 128)
        state = hash_blocks(state, powers, data, 8, nullptr);
    if (!bytes)
        return state;
    const u32 full = bytes / 16;
    u8 tail[16]{};
    copy_bytes(tail, data + full * 16, bytes & 15);
    return hash_blocks(state, powers, data, full, bytes & 15 ? tail : nullptr);
}
// Key storage: H^1..H^8 as twisted powers with their middle operands, then the
// round keys (11 for AES-128, 15 for AES-256).
constexpr u32 kPowerBytes = 8 * 32;
// Counter blocks in flight.
constexpr u32 kLanes = 8;
inline __m128i load_nonce(const u8* nonce) noexcept {
    return _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(nonce)), _mm_loadu_si32(nonce + 8));
}
inline __m128i counter_vector(__m128i nonce, u32 counter) noexcept {
    return _mm_or_si128(nonce,
                        _mm_slli_si128(_mm_cvtsi32_si128(static_cast<i32>(byte_swap(counter))), 12));
}
// The last 1-15 bytes: the keystream block goes through two general registers,
// so no keystream copy stays on the stack.
inline void xor_partial(const u8* input, u8* output, __m128i stream, u32 bytes) noexcept {
    u64 word = static_cast<u64>(_mm_cvtsi128_si64(stream));
    if (bytes >= 8) {
        byte_store_le64(output, byte_load_le64(input) ^ word);
        word = static_cast<u64>(_mm_cvtsi128_si64(_mm_unpackhi_epi64(stream, stream)));
        input += 8;
        output += 8;
        bytes -= 8;
    }
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
    for (u32 i = 0; i != bytes; ++i, word >>= 8)
        output[i] = static_cast<u8>(input[i] ^ word);
}
// `last` is the final round key: the schedule length selects AES-128 or AES-256.
template <u32 count> inline void aes_encrypt(const u8* keys, const u8* last, __m128i (&blocks)[count]) noexcept {
    auto round = _mm_loadu_si128(reinterpret_cast<const __m128i*>(keys));
    for (u32 i = 0; i != count; ++i)
        blocks[i] = _mm_xor_si128(blocks[i], round);
#pragma clang loop unroll(disable)
    for (keys += 16; keys != last; keys += 16) {
        round = _mm_loadu_si128(reinterpret_cast<const __m128i*>(keys));
        for (u32 i = 0; i != count; ++i)
            blocks[i] = _mm_aesenc_si128(blocks[i], round);
    }
    round = _mm_loadu_si128(reinterpret_cast<const __m128i*>(last));
    for (u32 i = 0; i != count; ++i)
        blocks[i] = _mm_aesenclast_si128(blocks[i], round);
}
// GCTR from `counter`, kLanes blocks at a time. The branches depend on the
// length only.
void ctr_transform(const u8* keys, const u8* last, __m128i nonce, u32 counter, const u8* source, u8* destination,
                   u32 bytes) noexcept {
    while (bytes) {
        __m128i stream[kLanes];
        for (u32 i = 0; i != kLanes; ++i)
            stream[i] = counter_vector(nonce, counter + i);
        aes_encrypt(keys, last, stream);
        counter += kLanes;
        for (u32 i = 0; i != kLanes; ++i) {
            if (bytes < 16) {
                xor_partial(source, destination, stream[i], bytes);
                return;
            }
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination),
                             _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source)), stream[i]));
            source += 16;
            destination += 16;
            bytes -= 16;
        }
    }
}
AeadResult crypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                 Slice<u8> output, ConstSlice<u8> tag, bool encrypt) noexcept {
    if (!key.storage.data || !key.storage.locked)
        return aead_result(AeadError::not_initialized);
    // Every caller passes a 12-byte nonce, an 8- or 16-byte tag and an output of the input's size, in its own
    // arrays (packet_crypto.cpp, dave/frame_crypto.cpp, mls/hpke.cpp, the Welcomes, startup_shell.cpp).
    // Storage: the GHASH powers, then the round keys up to the last one.
    const u8* powers = key.storage.data;
    const u8* keys = powers + kPowerBytes;
    const u8* last = powers + key.storage.capacity - 16;
    const auto nonce_vector = load_nonce(nonce.data);
    __m128i mask[1]{counter_vector(nonce_vector, 1)};
    aes_encrypt(keys, last, mask);
    auto state = hash_part(_mm_setzero_si128(), powers, aad.data, aad.count);
    if (encrypt) {
        ctr_transform(keys, last, nonce_vector, 2, input.data, output.data, input.count);
        state = hash_part(state, powers, output.data, input.count);
    } else {
        state = hash_part(state, powers, input.data, input.count);
    }
    // The closing block: bit lengths of the AAD (high half) and ciphertext.
    Sum closing{_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128()};
    accumulate(closing,
               _mm_xor_si128(state, _mm_unpacklo_epi64(_mm_cvtsi64_si128(static_cast<long long>(u64{input.count} * 8)),
                                                       _mm_cvtsi64_si128(static_cast<long long>(u64{aad.count} * 8)))),
               powers);
    const auto full = _mm_xor_si128(swap_bytes(reduce(closing)), mask[0]);
    auto* tag_data = reinterpret_cast<__m128i*>(const_cast<u8*>(tag.data));
    // The tag length is the caller's constant, not secret data. The 8-byte tag
    // is the first half of the full one.
    if (encrypt) {
        if (tag.count == 8)
            _mm_storel_epi64(tag_data, full);
        else
            _mm_storeu_si128(tag_data, full);
        return aead_result(AeadError::none, input.count);
    }
    // One test of all tag bytes: no early exit on the first different byte.
    const auto expected = tag.count == 8 ? _mm_loadl_epi64(tag_data) : _mm_loadu_si128(tag_data);
    const int equal = _mm_movemask_epi8(_mm_cmpeq_epi8(full, expected)) | (tag.count == 8 ? 0xff00 : 0);
    if (equal != 0xffff) {
        secure_clear_bytes(output.data, input.count);
        return aead_result(AeadError::authentication_failed);
    }
    ctr_transform(keys, last, nonce_vector, 2, input.data, output.data, input.count);
    return aead_result(AeadError::none, input.count);
}
} // namespace

// The key schedule and power table are built once per key: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
AeadResult aes_gcm_initialize(AesGcmKey* key, ConstSlice<u8> secret) noexcept {
    if (key->storage.data || key->storage.locked || (secret.count != 16 && secret.count != 32) ||
        !byte_range_valid(secret) || byte_ranges_overlap(secret, {reinterpret_cast<const u8*>(key), sizeof(*key)}))
        return aead_result(AeadError::invalid_input);
    // Round keys: 11 for 16 secret bytes (AES-128), 15 for 32 (AES-256).
    const u32 schedule = (secret.count / 4 + 7) * 16;
    const auto storage = secure_buffer_initialize(&key->storage, kPowerBytes + schedule);
    if (!storage.ok())
        return aead_result(AeadError::storage, 0, static_cast<i32>(storage.native_error));
    u8* powers = key->storage.data;
    u8* keys = powers + kPowerBytes;
    copy_bytes(keys, secret.data, secret.count);
    // FIPS 197 section 5.2, four words at a time. AESKEYGENASSIST with round
    // constant 0 gives SubWord(w3) in word 2 and RotWord(SubWord(w3)) in word 3
    // of the previous round key. The round constant and the key length are
    // public, so their branches do not depend on the secret.
    __m128i word = _mm_setzero_si128();
    u32 constant = 1;
    for (u32 at = secret.count; at != schedule; at += 16) {
        const auto assist =
            _mm_aeskeygenassist_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(keys + at - 16)), 0);
        auto mix = _mm_shuffle_epi32(assist, 0xaa);
        if (!(at & (secret.count - 1))) {
            mix = _mm_xor_si128(_mm_shuffle_epi32(assist, 0xff), _mm_set1_epi32(static_cast<i32>(constant)));
            constant = (constant << 1) ^ (constant >> 7) * 0x11b;
        }
        word = _mm_loadu_si128(reinterpret_cast<const __m128i*>(keys + at - secret.count));
        word = _mm_xor_si128(word, _mm_slli_si128(word, 4));
        word = _mm_xor_si128(word, _mm_slli_si128(word, 8));
        word = _mm_xor_si128(word, mix);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(keys + at), word);
    }
    secure_clear_bytes(&word, sizeof(word));
    __m128i zero[1]{_mm_setzero_si128()};
    aes_encrypt(keys, keys + schedule - 16, zero);
    auto power = swap_bytes(zero[0]);
    store_power(powers, power);
    for (u32 offset = 32; offset != kPowerBytes; offset += 32) {
        Sum next{_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128()};
        accumulate(next, power, powers);
        power = reduce(next);
        store_power(powers + offset, power);
    }
    secure_clear_bytes(&power, sizeof(power));
    secure_clear_bytes(zero, sizeof(zero));
    key->storage.bytes = key->storage.capacity;
    return aead_result(AeadError::none);
}
void aes_gcm_release(AesGcmKey* key) noexcept {
    secure_buffer_release(&key->storage);
}
#pragma clang attribute pop
AeadResult aes_gcm_encrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept {
    return crypt(key, nonce, aad, plaintext, ciphertext, {tag.data, tag.count}, true);
}
AeadResult aes_gcm_decrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept {
    return crypt(key, nonce, aad, ciphertext, plaintext, tag, false);
}
} // namespace maicord
