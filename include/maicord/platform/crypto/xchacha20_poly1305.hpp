#pragma once

#include "maicord/platform/crypto/aead.hpp"
#include "maicord/platform/crypto/secure_buffer.hpp"

namespace maicord {

// native is the operating system's ChaCha20-Poly1305 (CNG, or OpenSSL on Linux); portable is the in-house one,
// which also serves systems that lack the native cipher (older Windows 10 builds, a Linux without OpenSSL).
enum class XChaChaBackend : u8 {
    automatic,
    native,
    portable
};

// Zero-initialize; single worker ownership, no copying or concurrent operations.
// One locked allocation holds the native workspace, original key and derived
// key. The native workspace is reused, not allocated per packet. Only the original key
// survives an operation; temporary keys are erased. Backend selection is once
// at initialization, not a retry after an authentication or key-import failure.
struct XChaChaKey {
    SecureBuffer storage;
    u32 object_bytes;
    XChaChaBackend backend;
};

AeadResult xchacha_initialize(XChaChaKey* key, ConstSlice<u8> secret,
                              XChaChaBackend backend = XChaChaBackend::automatic) noexcept;
void xchacha_release(XChaChaKey* key) noexcept;

// 192-bit nonce, detached 128-bit tag. Caller owns per-key nonce uniqueness.
// Exact in-place payload operation and adjacent tags are supported. No argument
// may overlap key storage/owner. Partial payload overlap or writes into AAD,
// nonce or tag are rejected unchanged, as are malformed/short destinations.
// Native authentication failure erases the attempted output prefix; key-import
// failure leaves it unchanged. Both return zero bytes, never valid plaintext.
AeadResult xchacha_encrypt(XChaChaKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept;
AeadResult xchacha_decrypt(XChaChaKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept;

} // namespace maicord
