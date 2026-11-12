// aes_gcm.hpp over the system OpenSSL. The key owns two EVP_CIPHER_CTX objects because a context's direction is
// fixed when it is initialized: `handle` is the encrypting one and the locked object storage holds the pointer of the
// decrypting one. Each packet only re-initializes the IV (EVP_*Init_ex with the key left in place), so no packet
// allocates, derives a key schedule or opens a provider. The key schedules live in OpenSSL's heap, not in our locked
// pages (the one weaker property of this backend; EVP_CIPHER_CTX_free cleanses them on release).
#include "maicord/platform/crypto/aes_gcm.hpp"
#include "aead_openssl.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

AeadResult crypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                 Slice<u8> output, ConstSlice<u8> tag, bool encrypt) noexcept {
    if (!key.handle || !key.object.data || !key.object.locked)
        return aead_result(AeadError::not_initialized);
    const auto valid = aead_validate({reinterpret_cast<const u8*>(&key), sizeof(key)},
                                     {key.object.data, key.object.capacity}, 12, nonce, aad, input, output, tag,
                                     encrypt);
    if (!valid.ok())
        return valid;
    const openssl::Crypto* library = openssl::crypto();
    if (!library)
        return aead_result(AeadError::platform_failure, 0, openssl::library_status(openssl::kFailureLibrary));
    void* context = key.handle;
    if (!encrypt)
        copy_bytes(&context, key.object.data, sizeof(context));
    return openssl_aead_run(*library, context, nonce, aad, input, output, const_cast<u8*>(tag.data), encrypt);
}

} // namespace

// Key setup and release, once per key: compiled for size (the per-packet crypt above stays at full speed).
__attribute__((minsize)) AeadResult aes_gcm_initialize(AesGcmKey* key, ConstSlice<u8> secret) noexcept {
    if (!key || key->handle || key->object.data || key->object.locked || (secret.count != 16 && secret.count != 32) ||
        !byte_range_valid(secret) || byte_ranges_overlap(secret, {reinterpret_cast<const u8*>(key), sizeof(*key)}))
        return aead_result(AeadError::invalid_input);
    u32 failure = 0;
    const openssl::Crypto* library = openssl::crypto(&failure);
    if (!library)
        return aead_result(AeadError::platform_failure, 0, openssl::library_status(failure));
    const auto storage = secure_buffer_initialize(&key->object, sizeof(void*));
    if (!storage.ok())
        return aead_result(AeadError::storage, 0, static_cast<i32>(storage.native_error));
    const void* cipher = secret.count == 16 ? library->EVP_aes_128_gcm() : library->EVP_aes_256_gcm();
    void* encryptor = library->EVP_CIPHER_CTX_new();
    void* decryptor = library->EVP_CIPHER_CTX_new();
    const bool good =
        encryptor && decryptor &&
        library->EVP_EncryptInit_ex(encryptor, cipher, nullptr, nullptr, nullptr) == 1 &&
        library->EVP_CIPHER_CTX_ctrl(encryptor, openssl::kCtrlAeadSetIvLength, 12, nullptr) == 1 &&
        library->EVP_EncryptInit_ex(encryptor, nullptr, nullptr, secret.data, nullptr) == 1 &&
        library->EVP_DecryptInit_ex(decryptor, cipher, nullptr, nullptr, nullptr) == 1 &&
        library->EVP_CIPHER_CTX_ctrl(decryptor, openssl::kCtrlAeadSetIvLength, 12, nullptr) == 1 &&
        library->EVP_DecryptInit_ex(decryptor, nullptr, nullptr, secret.data, nullptr) == 1;
    if (!good) {
        const i32 status = openssl::error_status(*library);
        if (encryptor)
            library->EVP_CIPHER_CTX_free(encryptor);
        if (decryptor)
            library->EVP_CIPHER_CTX_free(decryptor);
        secure_buffer_release(&key->object);
        return aead_result(AeadError::platform_failure, 0, status);
    }
    copy_bytes(key->object.data, &decryptor, sizeof(decryptor));
    key->object.bytes = sizeof(decryptor);
    key->handle = encryptor;
    return aead_result(AeadError::none);
}

__attribute__((minsize)) void aes_gcm_release(AesGcmKey* key) noexcept {
    if (!key)
        return;
    if (key->handle) {
        const openssl::Crypto* library = openssl::crypto();
        if (library) {
            void* decryptor = nullptr;
            if (key->object.data)
                copy_bytes(&decryptor, key->object.data, sizeof(decryptor));
            library->EVP_CIPHER_CTX_free(key->handle);
            if (decryptor)
                library->EVP_CIPHER_CTX_free(decryptor);
        }
    }
    key->handle = nullptr;
    secure_buffer_release(&key->object);
}

AeadResult aes_gcm_encrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> plaintext,
                           Slice<u8> ciphertext, Slice<u8> tag) noexcept {
    return crypt(key, nonce, aad, plaintext, ciphertext, {tag.data, tag.count}, true);
}

AeadResult aes_gcm_decrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> ciphertext,
                           ConstSlice<u8> tag, Slice<u8> plaintext) noexcept {
    return crypt(key, nonce, aad, ciphertext, plaintext, tag, false);
}

} // namespace maicord
