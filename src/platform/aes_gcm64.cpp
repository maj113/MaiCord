#include "maicord/platform/aes_gcm64.hpp"
#include "maicord/core/memory.hpp"
#include "aead_internal.hpp"

#include "maicord/sys/crypto.hpp"
#include "maicord/sys/simd.hpp"

#pragma intrinsic(_byteswap_ulong)

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
// Register-only nibble lookup: secret data never selects a memory address.
inline __m128i reflect_bits(__m128i value) noexcept {
    const auto table = _mm_setr_epi8(0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15);
    const auto mask = _mm_set1_epi8(15);
    const auto low = _mm_shuffle_epi8(table, _mm_and_si128(value, mask));
    const auto high = _mm_shuffle_epi8(table, _mm_and_si128(_mm_srli_epi16(value, 4), mask));
    return _mm_or_si128(_mm_slli_epi16(low, 4), high);
}
struct Product {
    __m128i low, high;
};
inline Product product(__m128i a, __m128i b) noexcept {
    const auto ll = _mm_clmulepi64_si128(a, b, 0x00);
    const auto hh = _mm_clmulepi64_si128(a, b, 0x11);
    const auto cross = _mm_xor_si128(_mm_clmulepi64_si128(a, b, 0x01), _mm_clmulepi64_si128(a, b, 0x10));
    return {_mm_xor_si128(ll, _mm_slli_si128(cross, 8)), _mm_xor_si128(hh, _mm_srli_si128(cross, 8))};
}
template <u32 bits> inline __m128i shift_left(__m128i value) noexcept {
    return _mm_xor_si128(_mm_slli_epi64(value, bits), _mm_slli_si128(_mm_srli_epi64(value, 64 - bits), 8));
}
inline __m128i reduce(Product p) noexcept {
    // x^128 = x^7+x^2+x+1. The overflow of the first fold has at most seven
    // bits. Both folds stay in vector registers, with no bit-at-a-time loop.
    const auto upper_high = _mm_srli_si128(p.high, 8);
    const auto overflow =
        _mm_xor_si128(_mm_xor_si128(_mm_srli_epi64(upper_high, 63), _mm_srli_epi64(upper_high, 62)),
                      _mm_srli_epi64(upper_high, 57));
    const auto folded = _mm_xor_si128(_mm_xor_si128(p.high, shift_left<1>(p.high)),
                                      _mm_xor_si128(shift_left<2>(p.high), shift_left<7>(p.high)));
    const auto last =
        _mm_xor_si128(_mm_xor_si128(overflow, _mm_slli_epi64(overflow, 1)),
                      _mm_xor_si128(_mm_slli_epi64(overflow, 2), _mm_slli_epi64(overflow, 7)));
    return _mm_xor_si128(p.low, _mm_xor_si128(folded, last));
}
inline void accumulate(Product& sum, __m128i a, __m128i b) noexcept {
    const auto p = product(a, b);
    sum.low = _mm_xor_si128(sum.low, p.low);
    sum.high = _mm_xor_si128(sum.high, p.high);
}
inline __m128i wire_vector(const u8* bytes) noexcept {
    return reflect_bits(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes)));
}
inline __m128i hash_part_pclmul(__m128i state, const u8* powers, ConstSlice<u8> input) noexcept {
    const auto h1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(powers));
    if (input.count >= 64) {
        const auto h2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(powers + 16));
        const auto h3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(powers + 32));
        const auto h4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(powers + 48));
        do {
            // ((Y xor X0)*H^4 xor X1*H^3 xor X2*H^2 xor X3*H) mod P.
            // Independent carryless products, one reduction for all four blocks.
            auto sum = product(_mm_xor_si128(state, wire_vector(input.data)), h4);
            accumulate(sum, wire_vector(input.data + 16), h3);
            accumulate(sum, wire_vector(input.data + 32), h2);
            accumulate(sum, wire_vector(input.data + 48), h1);
            state = reduce(sum);
            input.data += 64;
            input.count -= 64;
        } while (input.count >= 64);
    }
    while (input.count >= 16) {
        state = reduce(product(_mm_xor_si128(state, wire_vector(input.data)), h1));
        input.data += 16;
        input.count -= 16;
    }
    if (input.count) {
        u8 tail[16]{};
        copy_bytes(tail, input.data, input.count);
        state = reduce(product(_mm_xor_si128(state, wire_vector(tail)), h1));
        SecureZeroMemory(tail, sizeof(tail));
    }
    return state;
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
        SecureZeroMemory(tail, sizeof(tail));
    }
    return state;
}
void store_be64(u8* bytes, u64 value) noexcept {
    for (u32 i = 0; i != 8; ++i)
        bytes[i] = static_cast<u8>(value >> (56 - i * 8));
}
void authenticate(const AesGcm64Key& key, ConstSlice<u8> aad, ConstSlice<u8> cipher, u8* mask) noexcept {
    u8 lengths[16];
    store_be64(lengths, u64{aad.count} * 8);
    store_be64(lengths + 8, u64{cipher.count} * 8);
    if (key.backend == Gcm64Backend::pclmul) {
        auto state = hash_part_pclmul(_mm_setzero_si128(), key.storage.data, aad);
        state = hash_part_pclmul(state, key.storage.data, cipher);
        const auto h1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key.storage.data));
        state = reflect_bits(reduce(product(_mm_xor_si128(state, wire_vector(lengths)), h1)));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(mask),
                         _mm_xor_si128(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(mask)), state));
        SecureZeroMemory(&state, sizeof(state));
    } else {
        Polynomial h;
        copy_bytes(&h, key.storage.data, 16);
        auto state = hash_part_portable({}, h, aad);
        state = hash_part_portable(state, h, cipher);
        state = block_portable(state, h, lengths);
        const u64 wire = reverse_bytes_bits(state.low);
        const auto* bytes = reinterpret_cast<const u8*>(&wire);
        for (u32 i = 0; i != 8; ++i)
            mask[i] ^= bytes[i];
        SecureZeroMemory(&h, sizeof(h));
        SecureZeroMemory(&state, sizeof(state));
    }
}
AeadResult aes_blocks(void* handle, u8* blocks, u32 bytes) noexcept {
    ULONG written = 0;
    const auto status =
        BCryptEncrypt(handle, blocks, bytes, nullptr, nullptr, 0, blocks, bytes, &written, 0);
    return status >= 0 && written == bytes ? aead_result(AeadError::none, bytes)
                                           : aead_result(AeadError::platform_failure, 0, status);
}
void counter_block(u8* block_bytes, ConstSlice<u8> nonce, u32 counter) noexcept {
    copy_bytes(block_bytes, nonce.data, 12);
    for (u32 i = 0; i != 4; ++i)
        block_bytes[12 + i] = static_cast<u8>(counter >> (24 - i * 8));
}
inline void write_counters(u8* output, __m128i nonce, u32 counter, u32 blocks) noexcept {
    for (u32 i = 0; i != blocks; ++i) {
        const auto suffix =
            _mm_slli_si128(_mm_cvtsi32_si128(static_cast<i32>(_byteswap_ulong(counter++))), 12);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + i * 16), _mm_or_si128(nonce, suffix));
    }
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
// With disjoint input/output, the caller's output is already a writable counter
// buffer. Encrypt all full blocks in one provider call; only J0/the partial tail
// need stack storage. No full-block keystream is copied through scratch.
AeadResult transform_disjoint(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> input,
                              Slice<u8> output, u8* mask, __m128i nonce_vector) noexcept {
    const u32 full_bytes = input.count & ~15u;
    const u32 tail_bytes = input.count - full_bytes;
    const u32 prefix = mask ? 16u : 0u;
    const u32 final_bytes = prefix + (tail_bytes ? 16u : 0u);
    u8 final_blocks[32];
    if (mask)
        counter_block(final_blocks, nonce, 1);
    if (tail_bytes)
        counter_block(final_blocks + prefix, nonce, full_bytes / 16 + 2);
    auto result =
        final_bytes ? aes_blocks(key.handle, final_blocks, final_bytes) : aead_result(AeadError::none);
    if (result.ok()) {
        write_counters(output.data, nonce_vector, 2, full_bytes / 16);
        result = aes_blocks(key.handle, output.data, full_bytes);
        if (result.ok()) {
            xor_stream(input.data, output.data, output.data, full_bytes);
            xor_stream(input.data + full_bytes, output.data + full_bytes, final_blocks + prefix,
                       tail_bytes);
            if (mask)
                copy_bytes(mask, final_blocks, 16);
        }
    }
    SecureZeroMemory(final_blocks, final_bytes);
    return result.ok() ? aead_result(AeadError::none, input.count) : result;
}
// Include J0 in the first encryption batch. All further blocks are GCTR,
// starting at counter 2. Bounded stack scratch; no frame-sized allocation.
AeadResult transform(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> input, Slice<u8> output,
                     u8* mask) noexcept {
    u8 initial[16];
    counter_block(initial, nonce, 0);
    const auto nonce_vector = _mm_loadu_si128(reinterpret_cast<const __m128i*>(initial));
    if (input.data != output.data && input.count >= 1024)
        return transform_disjoint(key, nonce, input, output, mask, nonce_vector);
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
        write_counters(stream + prefix, nonce_vector, counter, blocks);
        counter += blocks;
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
AeadResult crypt(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                 Slice<u8> output, ConstSlice<u8> tag, bool encrypt) noexcept {
    if (!key.handle || !key.storage.data || !key.storage.locked)
        return aead_result(AeadError::not_initialized);
    const auto valid = aead_validate({reinterpret_cast<const u8*>(&key), sizeof(key)},
                                     {key.storage.data, key.storage.capacity}, 12, nonce, aad, input,
                                     output, tag, encrypt, 8);
    if (!valid.ok())
        return valid;
    u8 mask[16];
    AeadResult result;
    if (encrypt) {
        result = transform(key, nonce, input, output, mask);
        if (result.ok()) {
            authenticate(key, aad, {output.data, input.count}, mask);
            copy_bytes(const_cast<u8*>(tag.data), mask, 8);
        }
    } else {
        counter_block(mask, nonce, 1);
        result = aes_blocks(key.handle, mask, 16);
        if (result.ok()) {
            authenticate(key, aad, input, mask);
            u32 mismatch = 0;
            for (u32 i = 0; i != 8; ++i)
                mismatch |= static_cast<u32>(mask[i] ^ tag.data[i]);
            result = mismatch ? aead_result(AeadError::authentication_failed)
                              : transform(key, nonce, input, output, nullptr);
        }
    }
    SecureZeroMemory(mask, sizeof(mask));
    if (!result.ok()) {
        if (input.count)
            SecureZeroMemory(output.data, input.count);
        if (encrypt)
            SecureZeroMemory(const_cast<u8*>(tag.data), 8);
    }
    return result;
}
} // namespace

AeadResult aes_gcm64_initialize(AesGcm64Key* key, ConstSlice<u8> secret, Gcm64Backend backend) noexcept {
    if (!key || key->handle || key->storage.data || key->storage.locked || secret.count != 16 ||
        !byte_range_valid(secret) ||
        byte_ranges_overlap(secret, {reinterpret_cast<const u8*>(key), sizeof(*key)}) ||
        (backend != Gcm64Backend::automatic && backend != Gcm64Backend::pclmul &&
         backend != Gcm64Backend::portable))
        return aead_result(AeadError::invalid_input);
    int cpu[4];
    __cpuid(cpu, 1);
    const bool pclmul = (cpu[2] & 0x202) == 0x202; // PCLMUL and SSSE3 register shuffles.
    if (backend == Gcm64Backend::pclmul && !pclmul)
        return aead_result(AeadError::platform_failure);
    const u32 hash_bytes = backend == Gcm64Backend::portable || !pclmul ? 16u : 64u;
    ULONG object_bytes = 0, returned = 0;
    auto status =
        BCryptGetProperty(BCRYPT_AES_ECB_ALG_HANDLE, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &returned, 0);
    if (status < 0 || returned != sizeof(object_bytes) || !object_bytes ||
        object_bytes > ~u32{0} - hash_bytes)
        return aead_result(AeadError::platform_failure, 0, status);
    const auto storage = secure_buffer_initialize(&key->storage, object_bytes + hash_bytes);
    if (!storage.ok())
        return aead_result(AeadError::storage, 0, static_cast<i32>(storage.native_error));
    status =
        BCryptGenerateSymmetricKey(BCRYPT_AES_ECB_ALG_HANDLE, &key->handle, key->storage.data + hash_bytes,
                                   object_bytes, const_cast<u8*>(secret.data), 16, 0);
    AeadResult result = status < 0 ? aead_result(AeadError::platform_failure, 0, status)
                                   : aes_blocks(key->handle, key->storage.data, 16);
    if (!result.ok()) {
        aes_gcm64_release(key);
        return result;
    }
    auto h = from_wire(key->storage.data);
    copy_bytes(key->storage.data, &h, 16);
    SecureZeroMemory(&h, sizeof(h));
    if (hash_bytes == 64) {
        const auto h1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key->storage.data));
        auto power = h1;
        for (u32 offset = 16; offset != 64; offset += 16) {
            power = reduce(product(power, h1));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(key->storage.data + offset), power);
        }
        SecureZeroMemory(&power, sizeof(power));
    }
    key->storage.bytes = key->storage.capacity;
    key->backend =
        backend == Gcm64Backend::portable || !pclmul ? Gcm64Backend::portable : Gcm64Backend::pclmul;
    return aead_result(AeadError::none);
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
