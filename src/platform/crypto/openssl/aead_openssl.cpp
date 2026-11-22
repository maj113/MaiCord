#include "openssl.hpp"
#include "../aead_internal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {

// Once per probe: compiled for size.
__attribute__((minsize)) bool aead_openssl_available() noexcept {
    return openssl::crypto() != nullptr;
}

// A new context for each packet: its init call fixes its direction, key and nonce. A failure after the init,
// including a tag mismatch, erases the attempted output prefix (and, when encrypting, the tag), returns zero bytes
// and is never valid plaintext. The voice and Go Live packets it seals fit in a datagram, far below INT_MAX.
AeadResult aead_openssl_chacha_crypt(const u8* key, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                                     Slice<u8> output, u8* tag, bool encrypt) noexcept {
    u32 failure = 0;
    const openssl::Crypto* library = openssl::crypto(&failure);
    if (!library)
        return aead_result(AeadError::platform_failure, 0, openssl::library_status(failure));
    void* context = library->EVP_CIPHER_CTX_new();
    if (!context)
        return aead_result(AeadError::platform_failure, 0, openssl::error_status(*library));
    const void* cipher = library->EVP_chacha20_poly1305();
    const bool keyed = encrypt ? library->EVP_EncryptInit_ex(context, cipher, nullptr, key, nonce.data) == 1
                               : library->EVP_DecryptInit_ex(context, cipher, nullptr, key, nonce.data) == 1;
    AeadResult result = aead_result(AeadError::none, input.count);
    if (!keyed) {
        result = aead_result(AeadError::platform_failure, 0, openssl::error_status(*library));
    } else {
        int length = 0;
        bool good = true;
        if (aad.count) {
            good = encrypt ? library->EVP_EncryptUpdate(context, nullptr, &length, aad.data,
                                                        static_cast<int>(aad.count)) == 1
                           : library->EVP_DecryptUpdate(context, nullptr, &length, aad.data,
                                                        static_cast<int>(aad.count)) == 1;
        }
        if (good && input.count) {
            good = encrypt ? library->EVP_EncryptUpdate(context, output.data, &length, input.data,
                                                        static_cast<int>(input.count)) == 1
                           : library->EVP_DecryptUpdate(context, output.data, &length, input.data,
                                                        static_cast<int>(input.count)) == 1;
            good = good && length == static_cast<int>(input.count);
        }
        // A decryption writes its plaintext before the tag is checked: the check is the final call, and its
        // failure is the only one reported as an authentication failure.
        bool authentic = true;
        if (good && !encrypt)
            good = library->EVP_CIPHER_CTX_ctrl(context, openssl::kCtrlAeadSetTag, 16, tag) == 1;
        if (good) {
            u8 trailing[16];
            int trailing_bytes = 0;
            const int final_result = encrypt ? library->EVP_EncryptFinal_ex(context, trailing, &trailing_bytes)
                                             : library->EVP_DecryptFinal_ex(context, trailing, &trailing_bytes);
            good = final_result == 1 && trailing_bytes == 0;
            authentic = !(final_result != 1 && !encrypt);
        }
        if (good && encrypt)
            good = library->EVP_CIPHER_CTX_ctrl(context, openssl::kCtrlAeadGetTag, 16, tag) == 1;
        if (!good) {
            // The status read also empties OpenSSL's error queue for the TLS calls that follow.
            const i32 status = openssl::error_status(*library);
            result = authentic ? aead_result(AeadError::platform_failure, 0, status)
                               : aead_result(AeadError::authentication_failed);
            secure_clear_bytes(output.data, input.count);
            if (encrypt)
                secure_clear_bytes(tag, 16);
        }
    }
    // EVP_CIPHER_CTX_free cleanses the key schedule it holds.
    library->EVP_CIPHER_CTX_free(context);
    return result;
}

} // namespace maicord
