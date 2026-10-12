#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kRemoteAuthPublicSpkiMaximumBytes = 320;
inline constexpr u32 kRemoteAuthSha256Bytes = 32;

enum class RemoteAuthCryptoError : u8 {
    none,
    invalid_input,
    not_initialized,
    output_too_small,
    platform_failure,
    invalid_key_blob,
};

struct RemoteAuthCryptoResult {
    u32 bytes;
    i32 platform_status;
    RemoteAuthCryptoError error;

    constexpr bool ok() const noexcept {
        return error == RemoteAuthCryptoError::none;
    }
};

// Holds one process-local ephemeral RSA-2048 key and the CNG providers needed
// by Remote Auth v2 (QR sign-in). The handles are opaque; callers must call
// remote_auth_crypto_release on every exit path.
struct RemoteAuthCrypto {
    void* rsa_algorithm;
    void* rsa_key;
    void* sha256_algorithm;
};

RemoteAuthCryptoResult remote_auth_crypto_initialize(
    RemoteAuthCrypto* crypto) noexcept;
void remote_auth_crypto_release(RemoteAuthCrypto* crypto) noexcept;

// Exports SubjectPublicKeyInfo DER using rsaEncryption plus a NULL parameter,
// as the Remote Auth init packet requires.
RemoteAuthCryptoResult remote_auth_crypto_export_spki(
    const RemoteAuthCrypto& crypto, Slice<u8> output) noexcept;
RemoteAuthCryptoResult remote_auth_crypto_sha256(
    const RemoteAuthCrypto& crypto, ConstSlice<u8> input,
    Slice<u8> digest) noexcept;
RemoteAuthCryptoResult remote_auth_crypto_fingerprint(
    const RemoteAuthCrypto& crypto, Slice<u8> digest) noexcept;

// Decrypts one RSA-2048 ciphertext with OAEP SHA-256. The destination is
// cleared if CNG rejects the ciphertext.
RemoteAuthCryptoResult remote_auth_crypto_decrypt(
    const RemoteAuthCrypto& crypto, ConstSlice<u8> ciphertext,
    Slice<u8> plaintext) noexcept;

} // namespace maicord
