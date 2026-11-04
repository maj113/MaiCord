// hash_stream.hpp on Windows: the CNG calls the digest users made themselves, behind forwarders that compile to the
// same call. Included by hash_stream.hpp.
#pragma once

#include "maicord/sys/crypto.hpp"

namespace maicord {

__forceinline i32 hash_stream_begin(HashStream* stream, HashAlgorithm algorithm) noexcept {
    return BCryptCreateHash(algorithm == HashAlgorithm::md5 ? BCRYPT_MD5_ALG_HANDLE : BCRYPT_SHA256_ALG_HANDLE, stream,
                            nullptr, 0, nullptr, 0, 0);
}

__forceinline i32 hash_stream_update(HashStream stream, const void* data, u32 bytes) noexcept {
    return BCryptHashData(stream, const_cast<u8*>(static_cast<const u8*>(data)), bytes, 0);
}

__forceinline i32 hash_stream_finish(HashStream stream, u8* digest, u32 digest_bytes) noexcept {
    return BCryptFinishHash(stream, digest, digest_bytes, 0);
}

__forceinline i32 hash_stream_destroy(HashStream stream) noexcept {
    return BCryptDestroyHash(stream);
}

} // namespace maicord
