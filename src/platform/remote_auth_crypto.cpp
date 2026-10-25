#include "maicord/platform/remote_auth_crypto.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/crypto.hpp"

namespace maicord {
namespace {

RemoteAuthCryptoResult result(RemoteAuthCryptoError error, u32 bytes = 0, NTSTATUS status = 0) noexcept {
    return RemoteAuthCryptoResult{bytes, static_cast<i32>(status), error};
}

bool initialized(const RemoteAuthCrypto& crypto) noexcept {
    return crypto.rsa_algorithm && crypto.rsa_key && crypto.sha256_algorithm;
}

u32 der_length_bytes(u32 length) noexcept {
    return length < 128 ? 1u : length <= 0xFF ? 2u : 3u;
}

u32 der_object_bytes(u32 content_bytes) noexcept {
    return 1u + der_length_bytes(content_bytes) + content_bytes;
}

u32 write_der_header(u8* output, u8 tag, u32 length) noexcept {
    output[0] = tag;
    if (length < 128) {
        output[1] = static_cast<u8>(length);
        return 2;
    }
    if (length <= 0xFF) {
        output[1] = 0x81;
        output[2] = static_cast<u8>(length);
        return 3;
    }
    output[1] = 0x82;
    output[2] = static_cast<u8>(length >> 8);
    output[3] = static_cast<u8>(length);
    return 4;
}

} // namespace

RemoteAuthCryptoResult remote_auth_crypto_initialize(RemoteAuthCrypto* crypto) noexcept {
    if (!crypto || crypto->rsa_algorithm || crypto->rsa_key || crypto->sha256_algorithm) {
        return result(RemoteAuthCryptoError::invalid_input);
    }

    BCRYPT_ALG_HANDLE rsa_algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&rsa_algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0);
    if (status < 0) {
        return result(RemoteAuthCryptoError::platform_failure, 0, status);
    }
    BCRYPT_ALG_HANDLE sha256_algorithm = nullptr;
    status = BCryptOpenAlgorithmProvider(&sha256_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(rsa_algorithm, 0);
        return result(RemoteAuthCryptoError::platform_failure, 0, status);
    }
    BCRYPT_KEY_HANDLE key = nullptr;
    status = BCryptGenerateKeyPair(rsa_algorithm, &key, 2048, 0);
    if (status >= 0) {
        status = BCryptFinalizeKeyPair(key, 0);
    }
    if (status < 0) {
        if (key) {
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(sha256_algorithm, 0);
        BCryptCloseAlgorithmProvider(rsa_algorithm, 0);
        return result(RemoteAuthCryptoError::platform_failure, 0, status);
    }

    crypto->rsa_algorithm = rsa_algorithm;
    crypto->rsa_key = key;
    crypto->sha256_algorithm = sha256_algorithm;
    return result(RemoteAuthCryptoError::none);
}

void remote_auth_crypto_release(RemoteAuthCrypto* crypto) noexcept {
    if (!crypto) {
        return;
    }
    if (crypto->rsa_key) {
        BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(crypto->rsa_key));
    }
    if (crypto->sha256_algorithm) {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(crypto->sha256_algorithm), 0);
    }
    if (crypto->rsa_algorithm) {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(crypto->rsa_algorithm), 0);
    }
    SecureZeroMemory(crypto, sizeof(*crypto));
}

RemoteAuthCryptoResult remote_auth_crypto_export_spki(const RemoteAuthCrypto& crypto,
                                                      Slice<u8> output) noexcept {
    if ((!output.data && output.count != 0)) {
        return result(RemoteAuthCryptoError::invalid_input);
    }
    if (!initialized(crypto)) {
        return result(RemoteAuthCryptoError::not_initialized);
    }

    ULONG blob_bytes = 0;
    NTSTATUS status = BCryptExportKey(static_cast<BCRYPT_KEY_HANDLE>(crypto.rsa_key), nullptr,
                                      BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &blob_bytes, 0);
    u8 blob[sizeof(BCRYPT_RSAKEY_BLOB) + 8 + 256]{};
    if (status < 0 || blob_bytes > sizeof(blob)) {
        return result(status < 0 ? RemoteAuthCryptoError::platform_failure
                                 : RemoteAuthCryptoError::invalid_key_blob,
                      0, status);
    }
    status = BCryptExportKey(static_cast<BCRYPT_KEY_HANDLE>(crypto.rsa_key), nullptr, BCRYPT_RSAPUBLIC_BLOB,
                             blob, blob_bytes, &blob_bytes, 0);
    if (status < 0) {
        SecureZeroMemory(blob, sizeof(blob));
        return result(RemoteAuthCryptoError::platform_failure, 0, status);
    }
    const auto* header = reinterpret_cast<const BCRYPT_RSAKEY_BLOB*>(blob);
    if (header->Magic != BCRYPT_RSAPUBLIC_MAGIC || header->BitLength != 2048 || header->cbModulus != 256 ||
        header->cbPublicExp == 0 || header->cbPublicExp > 8 ||
        blob_bytes != sizeof(BCRYPT_RSAKEY_BLOB) + header->cbPublicExp + header->cbModulus) {
        SecureZeroMemory(blob, sizeof(blob));
        return result(RemoteAuthCryptoError::invalid_key_blob);
    }

    const u8* exponent = blob + sizeof(BCRYPT_RSAKEY_BLOB);
    const u8* modulus = exponent + header->cbPublicExp;
    const u32 modulus_prefix = (modulus[0] & 0x80u) != 0 ? 1u : 0u;
    const u32 exponent_prefix = (exponent[0] & 0x80u) != 0 ? 1u : 0u;
    const u32 modulus_content = header->cbModulus + modulus_prefix;
    const u32 exponent_content = header->cbPublicExp + exponent_prefix;
    const u32 rsa_content = der_object_bytes(modulus_content) + der_object_bytes(exponent_content);
    const u32 rsa_bytes = der_object_bytes(rsa_content);
    const u32 bit_string_content = 1u + rsa_bytes;
    const u32 bit_string_bytes = der_object_bytes(bit_string_content);
    static constexpr u8 algorithm_identifier[] = {0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86,
                                           0xF7, 0x0D, 0x01, 0x01, 0x01, 0x05, 0x00};
    const u32 outer_content = sizeof(algorithm_identifier) + bit_string_bytes;
    const u32 required = der_object_bytes(outer_content);
    if (required > kRemoteAuthPublicSpkiMaximumBytes) {
        SecureZeroMemory(blob, sizeof(blob));
        return result(RemoteAuthCryptoError::invalid_key_blob);
    }
    if (required > output.count || (required != 0 && !output.data)) {
        SecureZeroMemory(blob, sizeof(blob));
        return result(RemoteAuthCryptoError::output_too_small, required);
    }

    u32 offset = write_der_header(output.data, 0x30, outer_content);
    copy_bytes(output.data + offset, algorithm_identifier, sizeof(algorithm_identifier));
    offset += sizeof(algorithm_identifier);
    offset += write_der_header(output.data + offset, 0x03, bit_string_content);
    output[offset++] = 0;
    offset += write_der_header(output.data + offset, 0x30, rsa_content);
    offset += write_der_header(output.data + offset, 0x02, modulus_content);
    if (modulus_prefix != 0) {
        output[offset++] = 0;
    }
    copy_bytes(output.data + offset, modulus, header->cbModulus);
    offset += header->cbModulus;
    offset += write_der_header(output.data + offset, 0x02, exponent_content);
    if (exponent_prefix != 0) {
        output[offset++] = 0;
    }
    copy_bytes(output.data + offset, exponent, header->cbPublicExp);
    offset += header->cbPublicExp;
    SecureZeroMemory(blob, sizeof(blob));
    return result(
        offset == required ? RemoteAuthCryptoError::none : RemoteAuthCryptoError::invalid_key_blob, offset);
}

RemoteAuthCryptoResult remote_auth_crypto_fingerprint(const RemoteAuthCrypto& crypto,
                                                      Slice<u8> digest) noexcept {
    u8 spki[kRemoteAuthPublicSpkiMaximumBytes]{};
    const RemoteAuthCryptoResult exported =
        remote_auth_crypto_export_spki(crypto, Slice<u8>{spki, static_cast<u32>(sizeof(spki))});
    if (!exported.ok()) {
        return exported;
    }
    RemoteAuthCryptoResult hashed;
    if (!digest.data && digest.count != 0) {
        hashed = result(RemoteAuthCryptoError::invalid_input);
    } else if (digest.count < kRemoteAuthSha256Bytes || !digest.data) {
        hashed = result(RemoteAuthCryptoError::output_too_small, kRemoteAuthSha256Bytes);
    } else {
        const NTSTATUS status =
            BCryptHash(static_cast<BCRYPT_ALG_HANDLE>(crypto.sha256_algorithm), nullptr, 0, spki,
                       exported.bytes, digest.data, kRemoteAuthSha256Bytes);
        hashed = status >= 0 ? result(RemoteAuthCryptoError::none, kRemoteAuthSha256Bytes, status)
                             : result(RemoteAuthCryptoError::platform_failure, 0, status);
    }
    SecureZeroMemory(spki, sizeof(spki));
    return hashed;
}

RemoteAuthCryptoResult remote_auth_crypto_decrypt(const RemoteAuthCrypto& crypto, ConstSlice<u8> ciphertext,
                                                  Slice<u8> plaintext) noexcept {
    if (!ciphertext.data || ciphertext.count != 256 || (!plaintext.data && plaintext.count != 0)) {
        return result(RemoteAuthCryptoError::invalid_input);
    }
    if (!initialized(crypto)) {
        return result(RemoteAuthCryptoError::not_initialized);
    }
    BCRYPT_OAEP_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, nullptr, 0};
    ULONG required = 0;
    NTSTATUS status =
        BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(crypto.rsa_key), const_cast<PUCHAR>(ciphertext.data),
                      ciphertext.count, &padding, nullptr, 0, nullptr, 0, &required, BCRYPT_PAD_OAEP);
    if (status < 0) {
        return result(RemoteAuthCryptoError::platform_failure, 0, status);
    }
    if (required > plaintext.count || (required != 0 && !plaintext.data)) {
        return result(RemoteAuthCryptoError::output_too_small, required);
    }
    ULONG written = 0;
    status = BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(crypto.rsa_key),
                           const_cast<PUCHAR>(ciphertext.data), ciphertext.count, &padding, nullptr, 0,
                           plaintext.data, plaintext.count, &written, BCRYPT_PAD_OAEP);
    if (status < 0) {
        if (plaintext.data && plaintext.count != 0) {
            SecureZeroMemory(plaintext.data, plaintext.count);
        }
        return result(RemoteAuthCryptoError::platform_failure, 0, status);
    }
    return result(RemoteAuthCryptoError::none, written, status);
}

} // namespace maicord
