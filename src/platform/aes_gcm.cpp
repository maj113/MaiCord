#include "maicord/platform/aes_gcm.hpp"
#include "aead_internal.hpp"

#include <Windows.h>
#include <bcrypt.h>

namespace maicord {
namespace {

AeadResult crypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                 Slice<u8> output, u8* tag, bool encrypt) noexcept {
    if (!key.handle || !key.object.data || !key.object.locked)
        return aead_result(AeadError::not_initialized);
    const auto valid = aead_validate({reinterpret_cast<const u8*>(&key), sizeof(key)},
                                     {key.object.data, key.object.capacity}, 12, nonce, aad, input, output,
                                     {tag, 16}, encrypt);
    if (!valid.ok())
        return valid;
    return aead_cng_crypt(key.handle, nonce, aad, input, output, tag, encrypt);
}

} // namespace

AeadResult aes_gcm_initialize(AesGcmKey* key, ConstSlice<u8> secret) noexcept {
    return aes_gcm_initialize_with_auxiliary(key, secret, 0);
}
AeadResult aes_gcm_initialize_with_auxiliary(AesGcmKey* key, ConstSlice<u8> secret,
                                             u32 auxiliary_bytes) noexcept {
    if (!key || key->handle || key->object.data || key->object.locked ||
        (secret.count != 16 && secret.count != 32) || !byte_range_valid(secret) ||
        byte_ranges_overlap(secret, {reinterpret_cast<const u8*>(key), sizeof(*key)}))
        return aead_result(AeadError::invalid_input);
    ULONG object_bytes = 0;
    ULONG returned = 0;
    NTSTATUS status =
        BCryptGetProperty(BCRYPT_AES_GCM_ALG_HANDLE, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &returned, 0);
    if (status < 0 || returned != sizeof(object_bytes) || !object_bytes)
        return aead_result(AeadError::platform_failure, 0, status);
    if (auxiliary_bytes > ~u32{0} - object_bytes)
        return aead_result(AeadError::invalid_input);
    const auto storage = secure_buffer_initialize(&key->object, object_bytes + auxiliary_bytes);
    if (!storage.ok())
        return aead_result(AeadError::storage, 0, static_cast<i32>(storage.native_error));
    status = BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &key->handle, key->object.data,
                                        object_bytes, const_cast<u8*>(secret.data), secret.count, 0);
    if (status < 0) {
        aes_gcm_release(key);
        return aead_result(AeadError::platform_failure, 0, status);
    }
    key->object.bytes = object_bytes;
    return aead_result(AeadError::none);
}

Slice<u8> aes_gcm_auxiliary(const AesGcmKey& key) noexcept {
    if (!key.handle || !key.object.data || !key.object.locked || key.object.bytes > key.object.capacity)
        return {};
    return {key.object.data + key.object.bytes, key.object.capacity - key.object.bytes};
}

void aes_gcm_release(AesGcmKey* key) noexcept {
    if (!key)
        return;
    if (key->handle)
        BCryptDestroyKey(key->handle);
    key->handle = nullptr;
    secure_buffer_release(&key->object);
}

AeadResult aes_gcm_encrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> plaintext, Slice<u8> ciphertext, Slice<u8> tag) noexcept {
    if (tag.count != kAesGcmTagBytes)
        return aead_result(AeadError::invalid_input);
    return crypt(key, nonce, aad, plaintext, ciphertext, tag.data, true);
}

AeadResult aes_gcm_decrypt(const AesGcmKey& key, ConstSlice<u8> nonce, ConstSlice<u8> aad,
                           ConstSlice<u8> ciphertext, ConstSlice<u8> tag, Slice<u8> plaintext) noexcept {
    if (tag.count != kAesGcmTagBytes)
        return aead_result(AeadError::invalid_input);
    return crypt(key, nonce, aad, ciphertext, plaintext, const_cast<u8*>(tag.data), false);
}

} // namespace maicord
