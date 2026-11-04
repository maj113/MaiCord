// remote_auth_crypto.hpp over the system OpenSSL: `rsa_key` is an EVP_PKEY (RSA-2048, e = 65537); the CNG provider
// handles of the Windows struct stay null. i2d_PUBKEY writes the SubjectPublicKeyInfo with rsaEncryption and a NULL
// parameter directly, where the Windows file builds it from the CNG blob.
#include "maicord/platform/crypto/remote_auth_crypto.hpp"
#include "openssl.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

RemoteAuthCryptoResult result(RemoteAuthCryptoError error, u32 bytes = 0, i32 status = 0) noexcept {
    return RemoteAuthCryptoResult{bytes, status, error};
}

RemoteAuthCryptoResult failure(const openssl::Crypto& library) noexcept {
    return result(RemoteAuthCryptoError::platform_failure, 0, openssl::error_status(library));
}

bool initialized(const RemoteAuthCrypto& crypto) noexcept {
    return crypto.rsa_key != nullptr;
}

} // namespace

RemoteAuthCryptoResult remote_auth_crypto_initialize(RemoteAuthCrypto* crypto) noexcept {
    if (!crypto || crypto->rsa_algorithm || crypto->rsa_key || crypto->sha256_algorithm)
        return result(RemoteAuthCryptoError::invalid_input);
    u32 reason = 0;
    const openssl::Crypto* library = openssl::crypto(&reason);
    if (!library)
        return result(RemoteAuthCryptoError::platform_failure, 0, openssl::library_status(reason));
    void* context = library->EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    void* key = nullptr;
    const bool good = context && library->EVP_PKEY_keygen_init(context) == 1 &&
                      library->EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) == 1 &&
                      library->EVP_PKEY_generate(context, &key) == 1 && key;
    const auto outcome = good ? result(RemoteAuthCryptoError::none) : failure(*library);
    if (context)
        library->EVP_PKEY_CTX_free(context);
    if (good)
        crypto->rsa_key = key;
    return outcome;
}

void remote_auth_crypto_release(RemoteAuthCrypto* crypto) noexcept {
    if (!crypto)
        return;
    if (crypto->rsa_key) {
        const openssl::Crypto* library = openssl::crypto();
        if (library)
            library->EVP_PKEY_free(crypto->rsa_key);
    }
    secure_clear_bytes(crypto, sizeof(*crypto));
}

RemoteAuthCryptoResult remote_auth_crypto_export_spki(const RemoteAuthCrypto& crypto, Slice<u8> output) noexcept {
    if (!output.data && output.count != 0)
        return result(RemoteAuthCryptoError::invalid_input);
    if (!initialized(crypto))
        return result(RemoteAuthCryptoError::not_initialized);
    const openssl::Crypto* library = openssl::crypto();
    if (!library)
        return result(RemoteAuthCryptoError::platform_failure, 0, openssl::library_status(openssl::kFailureLibrary));
    const int required = library->i2d_PUBKEY(crypto.rsa_key, nullptr);
    if (required <= 0)
        return failure(*library);
    if (static_cast<u32>(required) > kRemoteAuthPublicSpkiMaximumBytes)
        return result(RemoteAuthCryptoError::invalid_key_blob);
    if (static_cast<u32>(required) > output.count || !output.data)
        return result(RemoteAuthCryptoError::output_too_small, static_cast<u32>(required));
    u8 encoded[kRemoteAuthPublicSpkiMaximumBytes];
    u8* cursor = encoded;
    const int written = library->i2d_PUBKEY(crypto.rsa_key, &cursor);
    if (written != required || cursor != encoded + required)
        return result(RemoteAuthCryptoError::invalid_key_blob);
    copy_bytes(output.data, encoded, static_cast<u32>(written));
    return result(RemoteAuthCryptoError::none, static_cast<u32>(written));
}

RemoteAuthCryptoResult remote_auth_crypto_fingerprint(const RemoteAuthCrypto& crypto, Slice<u8> digest) noexcept {
    u8 spki[kRemoteAuthPublicSpkiMaximumBytes]{};
    const RemoteAuthCryptoResult exported =
        remote_auth_crypto_export_spki(crypto, Slice<u8>{spki, static_cast<u32>(sizeof(spki))});
    if (!exported.ok())
        return exported;
    RemoteAuthCryptoResult hashed;
    if (!digest.data && digest.count != 0) {
        hashed = result(RemoteAuthCryptoError::invalid_input);
    } else if (digest.count < kRemoteAuthSha256Bytes || !digest.data) {
        hashed = result(RemoteAuthCryptoError::output_too_small, kRemoteAuthSha256Bytes);
    } else {
        const openssl::Crypto* library = openssl::crypto();
        void* context = library ? library->EVP_MD_CTX_new() : nullptr;
        u8 sha[kRemoteAuthSha256Bytes];
        u32 length = 0;
        const bool good = context && library->EVP_DigestInit_ex(context, library->EVP_sha256(), nullptr) == 1 &&
                          library->EVP_DigestUpdate(context, spki, exported.bytes) == 1 &&
                          library->EVP_DigestFinal_ex(context, sha, &length) == 1 && length == kRemoteAuthSha256Bytes;
        if (good) {
            copy_bytes(digest.data, sha, kRemoteAuthSha256Bytes);
            hashed = result(RemoteAuthCryptoError::none, kRemoteAuthSha256Bytes);
        } else {
            hashed = library ? failure(*library)
                             : result(RemoteAuthCryptoError::platform_failure, 0,
                                      openssl::library_status(openssl::kFailureLibrary));
        }
        if (context)
            library->EVP_MD_CTX_free(context);
    }
    secure_clear_bytes(spki, sizeof(spki));
    return hashed;
}

RemoteAuthCryptoResult remote_auth_crypto_decrypt(const RemoteAuthCrypto& crypto, ConstSlice<u8> ciphertext,
                                                  Slice<u8> plaintext) noexcept {
    if (!ciphertext.data || ciphertext.count != 256 || (!plaintext.data && plaintext.count != 0))
        return result(RemoteAuthCryptoError::invalid_input);
    if (!initialized(crypto))
        return result(RemoteAuthCryptoError::not_initialized);
    const openssl::Crypto* library = openssl::crypto();
    if (!library)
        return result(RemoteAuthCryptoError::platform_failure, 0, openssl::library_status(openssl::kFailureLibrary));
    // OAEP-SHA-256 with MGF1-SHA-256 and no label. The plaintext of an RSA-2048 block is at most 190 bytes.
    u8 decrypted[256];
    usize written = sizeof(decrypted);
    void* context = library->EVP_PKEY_CTX_new(crypto.rsa_key, nullptr);
    const bool good = context && library->EVP_PKEY_decrypt_init(context) == 1 &&
                      library->EVP_PKEY_CTX_set_rsa_padding(context, openssl::kRsaOaepPadding) > 0 &&
                      library->EVP_PKEY_CTX_set_rsa_oaep_md(context, library->EVP_sha256()) > 0 &&
                      library->EVP_PKEY_CTX_set_rsa_mgf1_md(context, library->EVP_sha256()) > 0 &&
                      library->EVP_PKEY_decrypt(context, decrypted, &written, ciphertext.data, ciphertext.count) == 1;
    RemoteAuthCryptoResult outcome;
    if (!good) {
        outcome = failure(*library);
        if (plaintext.data && plaintext.count != 0)
            secure_clear_bytes(plaintext.data, plaintext.count);
    } else if (written > plaintext.count || (written != 0 && !plaintext.data)) {
        outcome = result(RemoteAuthCryptoError::output_too_small, static_cast<u32>(written));
    } else {
        copy_bytes(plaintext.data, decrypted, static_cast<u32>(written));
        outcome = result(RemoteAuthCryptoError::none, static_cast<u32>(written));
    }
    if (context)
        library->EVP_PKEY_CTX_free(context);
    secure_clear_bytes(decrypted, sizeof(decrypted));
    return outcome;
}

} // namespace maicord
