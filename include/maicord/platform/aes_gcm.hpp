#pragma once

#include "maicord/platform/aead.hpp"
#include "maicord/platform/secure_buffer.hpp"

namespace maicord {

inline constexpr u32 kAesGcmNonceBytes = 12;
inline constexpr u32 kAesGcmTagBytes = 16;

// Zero-initialize, do not copy, and release explicitly. A single worker uses
// the key and serializes operations/release. The CNG key object is locked and
// kept for the key's lifetime; packets do not open providers, allocate storage or regenerate keys.
struct AesGcmKey {
    void* handle;
    SecureBuffer object;
};

// Accepts exactly 16 or 32 key bytes. A live destination is rejected unchanged.
AeadResult aes_gcm_initialize(AesGcmKey* key, ConstSlice<u8> secret) noexcept;
void aes_gcm_release(AesGcmKey* key) noexcept;

// Fixed 96-bit nonce and detached 128-bit tag. The caller must guarantee nonce
// uniqueness for each encryption key, including across reconnects/counter wrap.
// In-place payload operation is supported; partial overlap is rejected.
// Arguments cannot overlap the key object; writes cannot overlap nonce/AAD/tag.
// Only the input-length prefix of output is used, so a trailing packet tag is allowed.
// Invalid arguments/short output leave destinations unchanged. A CNG failure
// erases the attempted output prefix (and encryption tag), returns zero bytes,
// and must never be interpreted as authenticated plaintext.
AeadResult aes_gcm_encrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept;
AeadResult aes_gcm_decrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept;

} // namespace maicord
