#pragma once

#include "openssl.hpp"
#include "../aead_internal.hpp"

namespace maicord {

// One AEAD operation on an EVP_CIPHER_CTX whose cipher and key are already set (and whose direction its init
// call fixed), the OpenSSL counterpart of aead_cng_crypt: 16-byte tag, payload processed in place or disjoint, a
// single call per packet. A failure, including a tag mismatch, erases the attempted output prefix (and, when
// encrypting, the tag), returns zero bytes and is never valid plaintext. Arguments are already validated; sizes
// above INT_MAX are rejected as invalid input.
AeadResult openssl_aead_run(const openssl::Crypto& library, void* context, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                            ConstSlice<u8> input, Slice<u8> output, u8* tag, bool encrypt) noexcept;

} // namespace maicord
