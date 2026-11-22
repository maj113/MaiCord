#pragma once

#include "maicord/platform/crypto/aead.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {

inline AeadResult aead_result(AeadError error, u32 bytes = 0, i32 status = 0) noexcept {
    return {bytes, status, error};
}

#if !defined(_WIN32)
// One ChaCha20-Poly1305 operation (12-byte nonce, 16-byte tag) under a 32-byte key, through the system OpenSSL
// (src/platform/crypto/openssl/aead_openssl.cpp). A failure erases the attempted output prefix.
AeadResult aead_openssl_chacha_crypt(const u8* key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                                     Slice<u8> output, u8* tag, bool encrypt) noexcept;
// Whether the system OpenSSL loads (xchacha_initialize refuses a key without it).
bool aead_openssl_available() noexcept;
#endif

} // namespace maicord
