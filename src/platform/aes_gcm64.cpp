#include "maicord/platform/aes_gcm64.hpp"
#include "maicord/core/memory.hpp"
#include "aead_internal.hpp"

#include "maicord/sys/crypto.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {
struct Polynomial {
    u64 low, high;
};

// GCM's first wire bit represents x^0. Reverse bits within each byte to put
// coefficients in the native little-endian order used by carryless multiply.
inline u64 reverse_bytes_bits(u64 x) noexcept {
    x = ((x & 0x5555555555555555ull) << 1) | ((x >> 1) & 0x5555555555555555ull);
    x = ((x & 0x3333333333333333ull) << 2) | ((x >> 2) & 0x3333333333333333ull);
    return ((x & 0x0f0f0f0f0f0f0f0full) << 4) | ((x >> 4) & 0x0f0f0f0f0f0f0f0full);
}
inline Polynomial from_wire(const u8* bytes) noexcept {
    const auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes));
    return {reverse_bytes_bits(static_cast<u64>(_mm_cvtsi128_si64(value))),
            reverse_bytes_bits(static_cast<u64>(_mm_cvtsi128_si64(_mm_srli_si128(value, 8))))};
}
Polynomial multiply_portable(Polynomial x, Polynomial h) noexcept {
    Polynomial z{};
    // Fixed iteration count, no secret-dependent branches or table accesses.
    for (u32 i = 0; i != 128; ++i) {
        const u64 select = 0 - (x.low & 1);
        z.low ^= h.low & select;
        z.high ^= h.high & select;
        x.low = (x.low >> 1) | (x.high << 63);
        x.high >>= 1;
        const u64 reduce = 0 - (h.high >> 63);
        h.high = (h.high << 1) | (h.low >> 63);
        h.low = (h.low << 1) ^ (reduce & 0x87);
    }
    return z;
}
// Accelerated GHASH. Blocks are byte-reversed ("Z form"), so bit 127-i holds
// the coefficient of x^i. Powers of H are kept twisted (H^k * x^-1 mod P) next
// to their Karatsuba middle operand, so the unshifted 255-bit carryless product
// of a block and a twisted power already has x^f at bit 255-f, and one
// reduction serves any number of blocks. Hashed data is AAD or ciphertext,
// both public, so partial-block copies are not wiped.
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
inline Polynomial block_portable(Polynomial state, Polynomial h, const u8* bytes) noexcept {
    const auto value = from_wire(bytes);
    return multiply_portable({state.low ^ value.low, state.high ^ value.high}, h);
}
inline Polynomial hash_part_portable(Polynomial state, Polynomial h, ConstSlice<u8> input) noexcept {
    while (input.count >= 16) {
        state = block_portable(state, h, input.data);
        input.data += 16;
        input.count -= 16;
    }
    if (input.count) {
        u8 tail[16]{};
        copy_bytes(tail, input.data, input.count);
        state = block_portable(state, h, tail);
    }
    return state;
}
// Accelerated storage: the AES-128 round keys, then H^1..H^8 as twisted powers
// with their middle operands. Portable storage: H, then the CNG key object.
constexpr u32 kRoundKeyBytes = 11 * 16, kPowerBytes = 8 * 32;
AeadResult aes_blocks(void* handle, u8* blocks, u32 bytes) noexcept {
    ULONG written = 0;
    const auto status =
        BCryptEncrypt(handle, blocks, bytes, nullptr, nullptr, 0, blocks, bytes, &written, 0);
    return status >= 0 && written == bytes ? aead_result(AeadError::none, bytes)
                                           : aead_result(AeadError::platform_failure, 0, status);
}
inline void counter_block(u8* block_bytes, ConstSlice<u8> nonce, u32 counter) noexcept {
    copy_bytes(block_bytes, nonce.data, 12);
    for (u32 i = 0; i != 4; ++i)
        block_bytes[12 + i] = static_cast<u8>(counter >> (24 - i * 8));
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
// Portable backend: CNG AES-ECB blocks and fixed-iteration GHASH. J0 is
// included in the first encryption batch; all further blocks are GCTR from
// counter 2. Bounded stack scratch; no frame-sized allocation.
AeadResult transform_portable(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> input,
                              Slice<u8> output, u8* mask) noexcept {
    u8 initial[16];
    counter_block(initial, nonce, 0);
    const auto nonce_vector = _mm_loadu_si128(reinterpret_cast<const __m128i*>(initial));
    u8 stream[1024];
    u32 offset = 0, counter = 2, used = 0;
    bool first = mask != nullptr;
    AeadResult result{};
    do {
        const u32 prefix = first ? 16u : 0u;
        const u32 bytes = minimum(input.count - offset, u32{sizeof(stream)} - prefix);
        const u32 blocks = (bytes + 15) / 16;
        if (first)
            counter_block(stream, nonce, 1);
        for (u32 i = 0; i != blocks; ++i)
            _mm_storeu_si128(reinterpret_cast<__m128i*>(stream + prefix + i * 16),
                             counter_vector(nonce_vector, counter++));
        if (!blocks && !first)
            break;
        used = maximum(used, prefix + blocks * 16);
        result = aes_blocks(key.handle, stream, prefix + blocks * 16);
        if (!result.ok())
            break;
        if (first)
            copy_bytes(mask, stream, 16);
        xor_stream(input.data + offset, output.data + offset, stream + prefix, bytes);
        offset += bytes;
        first = false;
    } while (offset != input.count);
    SecureZeroMemory(stream, used);
    return result.ok() ? aead_result(AeadError::none, input.count) : result;
}
AeadResult crypt_portable(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                          ConstSlice<u8> input, Slice<u8> output, ConstSlice<u8> tag, bool encrypt) noexcept {
    u8 mask[16];
    // GHASH over the AAD, then the ciphertext, then the lengths; its first 8
    // bytes mask into the tag.
    const auto authenticate = [&](ConstSlice<u8> cipher) noexcept {
        Polynomial h;
        copy_bytes(&h, key.storage.data, 16);
        // Big-endian bit lengths of the AAD and the ciphertext.
        const u64 closing[2]{_byteswap_uint64(u64{aad.count} * 8), _byteswap_uint64(u64{cipher.count} * 8)};
        auto state = hash_part_portable({}, h, aad);
        state = hash_part_portable(state, h, cipher);
        state = block_portable(state, h, reinterpret_cast<const u8*>(closing));
        const u64 wire = reverse_bytes_bits(state.low);
        const auto* bytes = reinterpret_cast<const u8*>(&wire);
        for (u32 i = 0; i != 8; ++i)
            mask[i] ^= bytes[i];
        SecureZeroMemory(&h, sizeof(h));
        SecureZeroMemory(&state, sizeof(state));
    };
    AeadResult result;
    if (encrypt) {
        result = transform_portable(key, nonce, input, output, mask);
        if (result.ok()) {
            authenticate({output.data, input.count});
            copy_bytes(const_cast<u8*>(tag.data), mask, 8);
        }
    } else {
        result = transform_portable(key, nonce, {}, {}, mask);
        if (result.ok()) {
            authenticate(input);
            u32 mismatch = 0;
            for (u32 i = 0; i != 8; ++i)
                mismatch |= static_cast<u32>(mask[i] ^ tag.data[i]);
            result = mismatch ? aead_result(AeadError::authentication_failed)
                              : transform_portable(key, nonce, input, output, nullptr);
        }
    }
    SecureZeroMemory(mask, sizeof(mask));
    return result;
}
// Accelerated backend: AES-128 with AES-NI, only reached after the CPUID check
// at initialization.
template <int rcon> MAICORD_TARGET_AES inline __m128i next_round_key(u8* destination, __m128i key) noexcept {
    const auto assist = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(key, rcon), 0xff);
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 8));
    key = _mm_xor_si128(key, assist);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination), key);
    return key;
}
template <u32 count> MAICORD_TARGET_AES inline void aes_encrypt(const u8* keys, __m128i (&blocks)[count]) noexcept {
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
MAICORD_TARGET_AES inline void ctr4(const u8* keys, __m128i nonce, u32 counter, const u8* source,
                                    u8* destination) noexcept {
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
MAICORD_TARGET_AES void ctr_transform(const u8* keys, __m128i nonce, u32 counter, const u8* source,
                                      u8* destination, u32 bytes) noexcept {
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
    SecureZeroMemory(bytes_stream, sizeof(bytes_stream));
}
MAICORD_TARGET_AES AeadResult crypt_accelerated(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                                                ConstSlice<u8> input, Slice<u8> output, ConstSlice<u8> tag,
                                                bool encrypt) noexcept {
    const u8* keys = key.storage.data;
    const u8* powers = keys + kRoundKeyBytes;
    u8 initial[16];
    counter_block(initial, nonce, 0);
    const auto nonce_vector = _mm_loadu_si128(reinterpret_cast<const __m128i*>(initial));
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
    if (_mm_cvtsi128_si64(_mm_xor_si128(full, _mm_loadl_epi64(reinterpret_cast<const __m128i*>(tag.data)))))
        return aead_result(AeadError::authentication_failed);
    ctr_transform(keys, nonce_vector, 2, input.data, output.data, input.count);
    return aead_result(AeadError::none, input.count);
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
    const auto result = key.backend == Gcm64Backend::accelerated
                            ? crypt_accelerated(key, nonce, aad, input, output, tag, encrypt)
                            : crypt_portable(key, nonce, aad, input, output, tag, encrypt);
    if (!result.ok()) {
        if (input.count)
            SecureZeroMemory(output.data, input.count);
        if (encrypt)
            SecureZeroMemory(const_cast<u8*>(tag.data), 8);
    }
    return result;
}
MAICORD_TARGET_AES AeadResult initialize_accelerated(AesGcm64Key* key, const u8* secret) noexcept {
    const auto storage = secure_buffer_initialize(&key->storage, kRoundKeyBytes + kPowerBytes);
    if (!storage.ok())
        return aead_result(AeadError::storage, 0, static_cast<i32>(storage.native_error));
    u8* keys = key->storage.data;
    auto round = _mm_loadu_si128(reinterpret_cast<const __m128i*>(secret));
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
    SecureZeroMemory(&round, sizeof(round));
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
    SecureZeroMemory(&power, sizeof(power));
    SecureZeroMemory(zero, sizeof(zero));
    key->storage.bytes = key->storage.capacity;
    key->backend = Gcm64Backend::accelerated;
    return aead_result(AeadError::none);
}
AeadResult initialize_portable(AesGcm64Key* key, const u8* secret) noexcept {
    ULONG object_bytes = 0, returned = 0;
    auto status =
        BCryptGetProperty(BCRYPT_AES_ECB_ALG_HANDLE, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &returned, 0);
    if (status < 0 || returned != sizeof(object_bytes) || !object_bytes || object_bytes > ~u32{0} - 16)
        return aead_result(AeadError::platform_failure, 0, status);
    const auto storage = secure_buffer_initialize(&key->storage, object_bytes + 16);
    if (!storage.ok())
        return aead_result(AeadError::storage, 0, static_cast<i32>(storage.native_error));
    status = BCryptGenerateSymmetricKey(BCRYPT_AES_ECB_ALG_HANDLE, &key->handle, key->storage.data + 16,
                                        object_bytes, const_cast<u8*>(secret), 16, 0);
    AeadResult result = status < 0 ? aead_result(AeadError::platform_failure, 0, status)
                                   : aes_blocks(key->handle, key->storage.data, 16);
    if (!result.ok()) {
        aes_gcm64_release(key);
        return result;
    }
    auto h = from_wire(key->storage.data);
    copy_bytes(key->storage.data, &h, 16);
    SecureZeroMemory(&h, sizeof(h));
    key->storage.bytes = key->storage.capacity;
    key->backend = Gcm64Backend::portable;
    return aead_result(AeadError::none);
}
} // namespace

AeadResult aes_gcm64_initialize(AesGcm64Key* key, ConstSlice<u8> secret, Gcm64Backend backend) noexcept {
    if (!key || key->handle || key->storage.data || key->storage.locked || secret.count != 16 ||
        !byte_range_valid(secret) ||
        byte_ranges_overlap(secret, {reinterpret_cast<const u8*>(key), sizeof(*key)}))
        return aead_result(AeadError::invalid_input);
    int cpu[4];
    __cpuid(cpu, 1);
    // AES-NI, PCLMULQDQ, and SSSE3 register shuffles.
    const bool accelerated = (cpu[2] & 0x2000202) == 0x2000202;
    if (backend == Gcm64Backend::accelerated && !accelerated)
        return aead_result(AeadError::platform_failure);
    return backend == Gcm64Backend::portable || !accelerated ? initialize_portable(key, secret.data)
                                                              : initialize_accelerated(key, secret.data);
}
void aes_gcm64_release(AesGcm64Key* key) noexcept {
    if (!key)
        return;
    if (key->handle)
        BCryptDestroyKey(key->handle);
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
