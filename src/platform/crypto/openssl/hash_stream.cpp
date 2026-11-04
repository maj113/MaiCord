// hash_stream.hpp over the system OpenSSL's EVP_MD: a stream is one EVP_MD_CTX.
#include "maicord/platform/crypto/hash_stream.hpp"
#include "openssl.hpp"

namespace maicord {

i32 hash_stream_begin(HashStream* stream, HashAlgorithm algorithm) noexcept {
    u32 failure = 0;
    const openssl::Crypto* library = openssl::crypto(&failure);
    if (!library)
        return openssl::library_status(failure);
    void* context = library->EVP_MD_CTX_new();
    const void* digest = algorithm == HashAlgorithm::md5 ? library->EVP_md5() : library->EVP_sha256();
    // MD5 can be disabled by a FIPS provider: the init then fails and the caller reports no fingerprint.
    if (!context || library->EVP_DigestInit_ex(context, digest, nullptr) != 1) {
        const i32 status = openssl::error_status(*library);
        if (context)
            library->EVP_MD_CTX_free(context);
        return status;
    }
    *stream = context;
    return 0;
}

i32 hash_stream_update(HashStream stream, const void* data, u32 bytes) noexcept {
    const openssl::Crypto* library = openssl::crypto();
    if (!library || !stream)
        return openssl::library_status(openssl::kFailureLibrary);
    if (bytes == 0)
        return 0;
    return library->EVP_DigestUpdate(stream, data, bytes) == 1 ? 0 : openssl::error_status(*library);
}

i32 hash_stream_finish(HashStream stream, u8* digest, u32 digest_bytes) noexcept {
    const openssl::Crypto* library = openssl::crypto();
    if (!library || !stream)
        return openssl::library_status(openssl::kFailureLibrary);
    u8 value[64];
    u32 length = 0;
    if (library->EVP_DigestFinal_ex(stream, value, &length) != 1)
        return openssl::error_status(*library);
    if (length != digest_bytes)
        return openssl::library_status(openssl::kFailureOutput);
    for (u32 i = 0; i != length; ++i)
        digest[i] = value[i];
    return 0;
}

i32 hash_stream_destroy(HashStream stream) noexcept {
    const openssl::Crypto* library = openssl::crypto();
    if (!library || !stream)
        return 0;
    library->EVP_MD_CTX_free(stream);
    return 0;
}

} // namespace maicord
