#pragma once

#include "maicord/platform/crypto/aead.hpp"
#include "maicord/platform/crypto/secure_buffer.hpp"

namespace maicord {

inline constexpr u32 kAesGcmNonceBytes = 12;
inline constexpr u32 kAesGcmTagBytes = 16;

// AES-GCM on AES-NI and PCLMULQDQ: the voice transport (AES-256), HPKE and the
// MLS welcome (AES-128) with a 128-bit tag, and DAVE frames (AES-128) with a
// 64-bit tag. Zero-initialize, do not copy, and release explicitly. A single
// worker uses the key and serializes operations/release. One locked allocation
// holds the GHASH subkey powers and the AES round keys; a live key has
// storage.data set. Packets do not allocate or expand keys.
struct AesGcmKey {
    SecureBuffer storage;
};

// Accepts exactly 16 or 32 key bytes. A live destination is rejected unchanged.
AeadResult aes_gcm_initialize(AesGcmKey* key, ConstSlice<u8> secret) noexcept;
void aes_gcm_release(AesGcmKey* key) noexcept;

// Fixed 96-bit nonce and a detached tag of exactly 16 or 8 bytes (DAVE). The
// caller fixes the tag length, so a peer cannot negotiate a shorter tag. The
// caller must guarantee nonce uniqueness for each encryption key, including
// across reconnects/counter wrap. In-place payload operation is supported; the
// caller passes no other overlap: no argument overlaps the key object or its
// storage, and writes do not overlap the payload, nonce, AAD or tag. Only the
// input-length prefix of output is used, so a trailing packet tag is allowed.
// Invalid arguments/short output leave destinations unchanged. Decryption
// verifies the complete tag before it produces any plaintext. A failed
// authentication erases the attempted output prefix, returns zero bytes and
// must never be interpreted as authenticated plaintext.
AeadResult aes_gcm_encrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept;
AeadResult aes_gcm_decrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept;

} // namespace maicord
