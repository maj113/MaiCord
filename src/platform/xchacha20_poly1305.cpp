#include "maicord/platform/xchacha20_poly1305.hpp"
#include "maicord/core/memory.hpp"
#include "aead_internal.hpp"

#include "maicord/sys/crypto.hpp"

namespace maicord {
namespace {

u32 load_le32(const u8* bytes) noexcept {
    return u32{bytes[0]} | (u32{bytes[1]} << 8) | (u32{bytes[2]} << 16) | (u32{bytes[3]} << 24);
}
void store_le32(u8* bytes, u32 value) noexcept {
    for (u32 i = 0; i != 4; ++i)
        bytes[i] = static_cast<u8>(value >> (i * 8));
}
u32 rotate(u32 value, u32 shift) noexcept {
    return (value << shift) | (value >> (32 - shift));
}

void quarter(u32& a, u32& b, u32& c, u32& d) noexcept {
    a += b;
    d = rotate(d ^ a, 16);
    c += d;
    b = rotate(b ^ c, 12);
    a += b;
    d = rotate(d ^ a, 8);
    c += d;
    b = rotate(b ^ c, 7);
}

void rounds(u32* state) noexcept {
    for (u32 round = 0; round != 10; ++round) {
        quarter(state[0], state[4], state[8], state[12]);
        quarter(state[1], state[5], state[9], state[13]);
        quarter(state[2], state[6], state[10], state[14]);
        quarter(state[3], state[7], state[11], state[15]);
        quarter(state[0], state[5], state[10], state[15]);
        quarter(state[1], state[6], state[11], state[12]);
        quarter(state[2], state[7], state[8], state[13]);
        quarter(state[3], state[4], state[9], state[14]);
    }
}

// HChaCha20 from draft-irtf-cfrg-xchacha-03 section 2.2. Unlike a ChaCha stream
// block it does not add the input state back after twenty rounds. Arithmetic
// and memory access are independent of secret values; no lookup tables.
void derive(const u8* secret, const u8* nonce, u8* subkey) noexcept {
    u32 state[16]{0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (u32 i = 0; i != 8; ++i)
        state[i + 4] = load_le32(secret + i * 4);
    for (u32 i = 0; i != 4; ++i)
        state[i + 12] = load_le32(nonce + i * 4);
    rounds(state);
    for (u32 i = 0; i != 4; ++i) {
        store_le32(subkey + i * 4, state[i]);
        store_le32(subkey + 16 + i * 4, state[i + 12]);
    }
    SecureZeroMemory(state, sizeof(state));
}

struct ChaChaStream {
    u32 initial[16], state[16];
    u8 block[64];
};

void stream_initialize(ChaChaStream& stream, const u8* secret, const u8* nonce) noexcept {
    auto& initial = stream.initial;
    initial[0] = 0x61707865;
    initial[1] = 0x3320646e;
    initial[2] = 0x79622d32;
    initial[3] = 0x6b206574;
    for (u32 i = 0; i != 8; ++i)
        initial[i + 4] = load_le32(secret + i * 4);
    initial[12] = 0;
    for (u32 i = 0; i != 3; ++i)
        initial[i + 13] = load_le32(nonce + i * 4);
}

void stream_block(ChaChaStream& stream, u32 counter) noexcept {
    stream.initial[12] = counter;
    copy_bytes(stream.state, stream.initial, sizeof(stream.state));
    rounds(stream.state);
    for (u32 i = 0; i != 16; ++i)
        store_le32(stream.block + i * 4, stream.state[i] + stream.initial[i]);
}

struct Poly1305 {
    u32 r[5], h[5], pad[4];
    u64 products[5];
};

void poly_initialize(Poly1305& poly, const u8* key) noexcept {
    reset_value(&poly);
    poly.r[0] = load_le32(key) & 0x3ffffff;
    poly.r[1] = (load_le32(key + 3) >> 2) & 0x3ffff03;
    poly.r[2] = (load_le32(key + 6) >> 4) & 0x3ffc0ff;
    poly.r[3] = (load_le32(key + 9) >> 6) & 0x3f03fff;
    poly.r[4] = (load_le32(key + 12) >> 8) & 0x00fffff;
    for (u32 i = 0; i != 4; ++i)
        poly.pad[i] = load_le32(key + 16 + i * 4);
}

// All AEAD MAC blocks are a full 16 bytes, including the zero-padded
// final AAD/ciphertext block and length block. Use radix 2^26 for arithmetic mod
// 2^130-5; each product sum fits in u64. Carries and final reduction use fixed
// arithmetic, not data-dependent branches or tables.
void poly_block(Poly1305& poly, const u8* input) noexcept {
    auto& h = poly.h;
    const auto& r = poly.r;
    h[0] += load_le32(input) & 0x3ffffff;
    h[1] += (load_le32(input + 3) >> 2) & 0x3ffffff;
    h[2] += (load_le32(input + 6) >> 4) & 0x3ffffff;
    h[3] += (load_le32(input + 9) >> 6) & 0x3ffffff;
    h[4] += (load_le32(input + 12) >> 8) | (1u << 24);
    auto& d = poly.products;
    d[0] =
        u64{h[0]} * r[0] + 5 * (u64{h[1]} * r[4] + u64{h[2]} * r[3] + u64{h[3]} * r[2] + u64{h[4]} * r[1]);
    d[1] =
        u64{h[0]} * r[1] + u64{h[1]} * r[0] + 5 * (u64{h[2]} * r[4] + u64{h[3]} * r[3] + u64{h[4]} * r[2]);
    d[2] =
        u64{h[0]} * r[2] + u64{h[1]} * r[1] + u64{h[2]} * r[0] + 5 * (u64{h[3]} * r[4] + u64{h[4]} * r[3]);
    d[3] = u64{h[0]} * r[3] + u64{h[1]} * r[2] + u64{h[2]} * r[1] + u64{h[3]} * r[0] + 5 * u64{h[4]} * r[4];
    d[4] = u64{h[0]} * r[4] + u64{h[1]} * r[3] + u64{h[2]} * r[2] + u64{h[3]} * r[1] + u64{h[4]} * r[0];
    for (u32 i = 0; i != 4; ++i) {
        h[i] = static_cast<u32>(d[i]) & 0x3ffffff;
        d[i + 1] += d[i] >> 26;
    }
    h[4] = static_cast<u32>(d[4]) & 0x3ffffff;
    h[0] += static_cast<u32>(d[4] >> 26) * 5;
    h[1] += h[0] >> 26;
    h[0] &= 0x3ffffff;
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
        SecureZeroMemory(last, sizeof(last));
    }
}

void poly_finish(Poly1305& poly, u8* tag) noexcept {
    auto& h = poly.h;
    for (u32 i = 1; i != 4; ++i) {
        h[i + 1] += h[i] >> 26;
        h[i] &= 0x3ffffff;
    }
    h[0] += (h[4] >> 26) * 5;
    h[4] &= 0x3ffffff;
    h[1] += h[0] >> 26;
    h[0] &= 0x3ffffff;
    u32 reduced[5], carry = 5;
    for (u32 i = 0; i != 5; ++i) {
        reduced[i] = h[i] + carry;
        carry = reduced[i] >> 26;
        reduced[i] &= 0x3ffffff;
    }
    // A carry out of bit 130 means h >= 2^130-5; choose h+5 modulo 2^130.
    const u32 mask = 0u - carry;
    for (u32 i = 0; i != 5; ++i)
        h[i] = (h[i] & ~mask) | (reduced[i] & mask);
    u32 words[4]{h[0] | (h[1] << 26), (h[1] >> 6) | (h[2] << 20), (h[2] >> 12) | (h[3] << 14),
                 (h[3] >> 18) | (h[4] << 8)};
    u64 sum = 0;
    for (u32 i = 0; i != 4; ++i) {
        sum = u64{words[i]} + poly.pad[i] + (sum >> 32);
        store_le32(tag + i * 4, static_cast<u32>(sum));
    }
    SecureZeroMemory(reduced, sizeof(reduced));
    SecureZeroMemory(words, sizeof(words));
    SecureZeroMemory(&poly, sizeof(poly));
}

void authenticate(Poly1305& poly, ConstSlice<u8> aad, ConstSlice<u8> ciphertext, u8* tag) noexcept {
    poly_padded(poly, aad);
    poly_padded(poly, ciphertext);
    u8 lengths[16]{};
    // Slice lengths are u32; the high half of each RFC 8439 u64 stays zero.
    store_le32(lengths, aad.count);
    store_le32(lengths + 8, ciphertext.count);
    poly_block(poly, lengths);
    poly_finish(poly, tag);
}

void xor_stream(ChaChaStream& stream, ConstSlice<u8> input, Slice<u8> output) noexcept {
    u32 counter = 1, offset = 0;
    while (offset != input.count) {
        stream_block(stream, counter++);
        const u32 count = minimum(64u, input.count - offset);
        for (u32 i = 0; i != count; ++i)
            output.data[offset + i] = input.data[offset + i] ^ stream.block[i];
        offset += count;
    }
}

AeadResult portable_crypt(const u8* subkey, const u8* nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                          Slice<u8> output, ConstSlice<u8> tag, bool encrypt) noexcept {
    ChaChaStream stream;
    stream_initialize(stream, subkey, nonce);
    stream_block(stream, 0);
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
        SecureZeroMemory(expected, sizeof(expected));
        if (difference) {
            SecureZeroMemory(&stream, sizeof(stream));
            if (input.count)
                SecureZeroMemory(output.data, input.count);
            return aead_result(AeadError::authentication_failed);
        }
        xor_stream(stream, input, output);
    }
    SecureZeroMemory(&stream, sizeof(stream));
    return aead_result(AeadError::none, input.count);
}

AeadResult crypt(XChaChaKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                 Slice<u8> output, ConstSlice<u8> tag, bool encrypt) noexcept {
    if (!key.storage.data || !key.storage.locked || key.storage.capacity < 64 ||
        key.object_bytes != key.storage.capacity - 64 ||
        (key.backend != XChaChaBackend::cng && key.backend != XChaChaBackend::portable))
        return aead_result(AeadError::not_initialized);
    const auto valid = aead_validate({reinterpret_cast<const u8*>(&key), sizeof(key)},
                                     {key.storage.data, key.storage.capacity}, 24, nonce, aad, input,
                                     output, tag, encrypt);
    if (!valid.ok())
        return valid;
    u8* secret = key.storage.data + key.object_bytes;
    u8* subkey = secret + 32;
    derive(secret, nonce.data, subkey);
    u8 ietf_nonce[12]{};
    copy_bytes(ietf_nonce + 4, nonce.data + 16, 8);
    if (key.backend == XChaChaBackend::portable) {
        const auto result = portable_crypt(subkey, ietf_nonce, aad, input, output, tag, encrypt);
        SecureZeroMemory(subkey, 32);
        return result;
    }
    BCRYPT_KEY_HANDLE handle = nullptr;
    const NTSTATUS status = BCryptGenerateSymmetricKey(BCRYPT_CHACHA20_POLY1305_ALG_HANDLE, &handle,
                                                       key.storage.data, key.object_bytes, subkey, 32, 0);
    SecureZeroMemory(subkey, 32);
    AeadResult result = aead_result(AeadError::platform_failure, 0, status);
    if (status >= 0) {
        result = aead_cng_crypt(handle, {ietf_nonce, 12}, aad, input, output, const_cast<u8*>(tag.data),
                                encrypt);
    }
    // Destroy even a partially initialized handle before erasing its object.
    if (handle)
        BCryptDestroyKey(handle);
    SecureZeroMemory(key.storage.data, key.object_bytes);
    return result;
}
} // namespace

AeadResult xchacha_initialize(XChaChaKey* key, ConstSlice<u8> secret, XChaChaBackend backend) noexcept {
    if (!key || key->storage.data || key->storage.locked || secret.count != 32 ||
        !byte_range_valid(secret) ||
        byte_ranges_overlap(secret, {reinterpret_cast<const u8*>(key), sizeof(*key)}) ||
        (backend != XChaChaBackend::automatic && backend != XChaChaBackend::cng &&
         backend != XChaChaBackend::portable))
        return aead_result(AeadError::invalid_input);
    ULONG bytes = 0, returned = 0;
    if (backend != XChaChaBackend::portable) {
        const NTSTATUS status =
            BCryptGetProperty(BCRYPT_CHACHA20_POLY1305_ALG_HANDLE, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&bytes), sizeof(bytes), &returned, 0);
        const bool unavailable = status == static_cast<NTSTATUS>(0xc0000008u) ||
                                 status == static_cast<NTSTATUS>(0xc00000bbu) ||
                                 status == static_cast<NTSTATUS>(0xc0000225u);
        if (backend == XChaChaBackend::automatic && unavailable) {
            backend = XChaChaBackend::portable;
            bytes = 0;
        } else {
            if (status < 0 || returned != sizeof(bytes) || !bytes || bytes > ~u32{0} - 64)
                return aead_result(AeadError::platform_failure, 0, status);
            backend = XChaChaBackend::cng;
        }
    }
    const auto allocated = secure_buffer_initialize(&key->storage, bytes + 64);
    if (!allocated.ok())
        return aead_result(AeadError::storage, 0, static_cast<i32>(allocated.native_error));
    copy_bytes(key->storage.data + bytes, secret.data, 32);
    key->storage.bytes = key->storage.capacity;
    key->object_bytes = bytes;
    key->backend = backend;
    return aead_result(AeadError::none);
}

void xchacha_release(XChaChaKey* key) noexcept {
    if (key) {
        secure_buffer_release(&key->storage);
        key->object_bytes = 0;
        key->backend = XChaChaBackend::automatic;
    }
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
