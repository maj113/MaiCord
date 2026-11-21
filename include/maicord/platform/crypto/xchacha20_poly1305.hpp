#pragma once

#include "maicord/platform/crypto/aead.hpp"
#include "maicord/platform/crypto/secure_buffer.hpp"

namespace maicord {

// The cipher is the in-house ChaCha20-Poly1305 on Windows and the system OpenSSL on Linux (a missing OpenSSL is
// platform_failure there). Zero-initialize; single worker ownership, no copying or concurrent operations. One locked
// allocation holds the original key and the derived key. Only the original key survives an operation; the derived
// key is erased.
struct XChaChaKey {
    SecureBuffer storage;
};

AeadResult xchacha_initialize(XChaChaKey* key, ConstSlice<u8> secret) noexcept;
void xchacha_release(XChaChaKey* key) noexcept;

// 192-bit nonce, detached 128-bit tag. Caller owns per-key nonce uniqueness.
// Exact in-place payload operation and adjacent tags are supported. No argument
// may overlap key storage/owner. Partial payload overlap or writes into AAD,
// nonce or tag are rejected unchanged, as are malformed/short destinations.
// Authentication failure erases the attempted output prefix and returns zero
// bytes, never valid plaintext.
AeadResult xchacha_encrypt(XChaChaKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept;
AeadResult xchacha_decrypt(XChaChaKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept;

} // namespace maicord
