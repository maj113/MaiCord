#pragma once

#include "maicord/platform/crypto/aead.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {

inline AeadResult aead_result(AeadError error, u32 bytes = 0, i32 status = 0) noexcept {
    return {bytes, status, error};
}

// Both algorithms share argument/alias rules and native failure erasure. The
// initialized owner must validate before deriving/importing or changing storage.
AeadResult aead_validate(ConstSlice<u8> owner, ConstSlice<u8> storage, u32 nonce_bytes,
                         ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input, Slice<u8> output,
                         ConstSlice<u8> tag, bool encrypt, u32 tag_bytes = 16) noexcept;
#if defined(_WIN32)
AeadResult aead_cng_crypt(void* handle, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                          Slice<u8> output, u8* tag, bool encrypt) noexcept;
#else
// One ChaCha20-Poly1305 operation (12-byte nonce, 16-byte tag) under a 32-byte key, through the system OpenSSL
// (src/platform/crypto/openssl/chacha.cpp); same failure erasure as aead_cng_crypt.
AeadResult aead_openssl_chacha_crypt(const u8* key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                                     Slice<u8> output, u8* tag, bool encrypt) noexcept;
// Whether the system OpenSSL loads (decides XChaChaBackend::automatic).
bool aead_openssl_available() noexcept;
#endif

} // namespace maicord
