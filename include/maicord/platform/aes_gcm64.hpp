#pragma once

#include "maicord/platform/aead.hpp"
#include "maicord/platform/secure_buffer.hpp"

namespace maicord {

// AES128-GCM with a fixed 96-bit nonce and 64-bit tag, as required by DAVE, on
// AES-NI and PCLMULQDQ. Independent from the transport cipher: its 128-bit tag
// is never shortened. Zero-initialize; one worker performs operations/release;
// never copy a live key. One locked allocation holds the AES round keys and
// GHASH subkey powers; a live key has storage.data set.
struct AesGcm64Key {
    SecureBuffer storage;
};

AeadResult aes_gcm64_initialize(AesGcm64Key* key, ConstSlice<u8> secret) noexcept;
void aes_gcm64_release(AesGcm64Key* key) noexcept;

// Caller guarantees per-key nonce uniqueness and enforces DAVE key lifetime,
// replay and failed-authentication policy. Exactly 16 secret / 12 nonce / 8 tag
// bytes; length is not negotiated by accepting an arbitrary shorter tag.
// Same alias/capacity contract as aes_gcm; in-place payload is supported.
// Invalid inputs leave output unchanged. Authentication failure wipes only the
// attempted payload prefix. Decryption verifies the complete tag before
// producing any plaintext. No per-frame allocation.
AeadResult aes_gcm64_encrypt(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                             ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept;
AeadResult aes_gcm64_decrypt(const AesGcm64Key& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                             ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept;

} // namespace maicord
