#include "maicord/platform/aes_gcm64.hpp"
#include "maicord/core/memory.hpp"
#include "aead_internal.hpp"

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
// Key storage: the AES-128 round keys, then H^1..H^8 as twisted powers with their
// middle operands.
constexpr u32 kRoundKeyBytes = 11 * 16, kPowerBytes = 8 * 32;
inline __m128i load_nonce(const u8* nonce) noexcept {
    return _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(nonce)), _mm_loadu_si32(nonce + 8));
}
inline __m128i counter_vector(__m128i nonce, u32 counter) noexcept {
    return _mm_or_si128(nonce,
                        _mm_slli_si128(_mm_cvtsi32_si128(static_cast<i32>(_byteswap_ulong(counter))), 12));
}
inline void xor_stream(const u8* input, u8* output, const u8* stream, u32 bytes) noexcept {
    u32 i = 0;
    for (; bytes - i >= 16; i += 16) {
        const auto source = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + i));
        const auto key_stream = _mm_loadu_si128(reinterpret_cast<const __m128i*>(stream + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + i), _mm_xor_si128(source, key_stream));
    }
    for (; i != bytes; ++i)
        output[i] = input[i] ^ stream[i];
}
template <int rcon> inline __m128i next_round_key(u8* destination, __m128i key) noexcept {
    const auto assist = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(key, rcon), 0xff);
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 8));
    key = _mm_xor_si128(key, assist);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination), key);
    return key;
}
template <u32 count> inline void aes_encrypt(const u8* keys, __m128i (&blocks)[count]) noexcept {
    auto round = _mm_loadu_si128(reinterpret_cast<const __m128i*>(keys));
    for (u32 i = 0; i != count; ++i)
        blocks[i] = _mm_xor_si128(blocks[i], round);
    for (u32 index = 1; index != 10; ++index) {
        round = _mm_loadu_si128(reinterpret_cast<const __m128i*>(keys + index * 16));
        for (u32 i = 0; i != count; ++i)
            blocks[i] = _mm_aesenc_si128(blocks[i], round);
    }
    round = _mm_loadu_si128(reinterpret_cast<const __m128i*>(keys + 160));
    for (u32 i = 0; i != count; ++i)
        blocks[i] = _mm_aesenclast_si128(blocks[i], round);
}
// Four counter blocks in flight.
inline void ctr4(const u8* keys, __m128i nonce, u32 counter, const u8* source, u8* destination) noexcept {
    __m128i stream[4]{counter_vector(nonce, counter), counter_vector(nonce, counter + 1),
                      counter_vector(nonce, counter + 2), counter_vector(nonce, counter + 3)};
    aes_encrypt(keys, stream);
    for (u32 i = 0; i != 4; ++i)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i * 16),
                         _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source + i * 16)),
                                       stream[i]));
}
// GCTR from `counter`. The final 1-63 bytes use one more group and a wiped
// stack keystream.
void ctr_transform(const u8* keys, __m128i nonce, u32 counter, const u8* source, u8* destination,
                   u32 bytes) noexcept {
    for (; bytes >= 64; bytes -= 64, source += 64, destination += 64, counter += 4)
        ctr4(keys, nonce, counter, source, destination);
    if (!bytes)
        return;
    __m128i stream[4]{counter_vector(nonce, counter), counter_vector(nonce, counter + 1),
                      counter_vector(nonce, counter + 2), counter_vector(nonce, counter + 3)};
    aes_encrypt(keys, stream);
    u8 bytes_stream[64];
    for (u32 i = 0; i != 4; ++i)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(bytes_stream + i * 16), stream[i]);
    xor_stream(source, destination, bytes_stream, bytes);
    secure_clear_bytes(bytes_stream, sizeof(bytes_stream));
}
AeadResult crypt(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                 Slice<u8> output, ConstSlice<u8> tag, bool encrypt) noexcept {
    if (!key.storage.data || !key.storage.locked)
        return aead_result(AeadError::not_initialized);
    const auto valid = aead_validate({reinterpret_cast<const u8*>(&key), sizeof(key)},
                                     {key.storage.data, key.storage.capacity}, 12, nonce, aad, input,
                                     output, tag, encrypt, 8);
    if (!valid.ok())
        return valid;
    const u8* keys = key.storage.data;
    const u8* powers = keys + kRoundKeyBytes;
    const auto nonce_vector = load_nonce(nonce.data);
    __m128i mask[1]{counter_vector(nonce_vector, 1)};
    aes_encrypt(keys, mask);
    auto state = hash_part(_mm_setzero_si128(), powers, aad.data, aad.count);
    if (encrypt) {
        ctr_transform(keys, nonce_vector, 2, input.data, output.data, input.count);
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
    if (encrypt) {
        _mm_storel_epi64(reinterpret_cast<__m128i*>(const_cast<u8*>(tag.data)), full);
        return aead_result(AeadError::none, input.count);
    }
    if (_mm_cvtsi128_si64(_mm_xor_si128(full, _mm_loadl_epi64(reinterpret_cast<const __m128i*>(tag.data))))) {
        secure_clear_bytes(output.data, input.count);
        return aead_result(AeadError::authentication_failed);
    }
    ctr_transform(keys, nonce_vector, 2, input.data, output.data, input.count);
    return aead_result(AeadError::none, input.count);
}
} // namespace

AeadResult aes_gcm64_initialize(AesGcm64Key* key, ConstSlice<u8> secret) noexcept {
    if (!key || key->storage.data || key->storage.locked || secret.count != 16 || !byte_range_valid(secret) ||
        byte_ranges_overlap(secret, {reinterpret_cast<const u8*>(key), sizeof(*key)}))
        return aead_result(AeadError::invalid_input);
    const auto storage = secure_buffer_initialize(&key->storage, kRoundKeyBytes + kPowerBytes);
    if (!storage.ok())
        return aead_result(AeadError::storage, 0, static_cast<i32>(storage.native_error));
    u8* keys = key->storage.data;
    auto round = _mm_loadu_si128(reinterpret_cast<const __m128i*>(secret.data));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(keys), round);
    round = next_round_key<0x01>(keys + 16, round);
    round = next_round_key<0x02>(keys + 32, round);
    round = next_round_key<0x04>(keys + 48, round);
    round = next_round_key<0x08>(keys + 64, round);
    round = next_round_key<0x10>(keys + 80, round);
    round = next_round_key<0x20>(keys + 96, round);
    round = next_round_key<0x40>(keys + 112, round);
    round = next_round_key<0x80>(keys + 128, round);
    round = next_round_key<0x1b>(keys + 144, round);
    round = next_round_key<0x36>(keys + 160, round);
    secure_clear_bytes(&round, sizeof(round));
    __m128i zero[1]{_mm_setzero_si128()};
    aes_encrypt(keys, zero);
    auto power = swap_bytes(zero[0]);
    u8* powers = keys + kRoundKeyBytes;
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
void aes_gcm64_release(AesGcm64Key* key) noexcept {
    if (!key)
        return;
    secure_buffer_release(&key->storage);
    reset_value(key);
}
AeadResult aes_gcm64_encrypt(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                             ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept {
    return crypt(key, nonce, aad, plaintext, ciphertext, {tag.data, tag.count}, true);
}
AeadResult aes_gcm64_decrypt(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                             ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept {
    return crypt(key, nonce, aad, ciphertext, plaintext, tag, false);
}
} // namespace maicord
