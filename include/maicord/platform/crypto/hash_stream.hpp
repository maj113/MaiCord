#pragma once

#include "maicord/core/base.hpp"

// A hash computed over data that arrives in pieces (a file read in chunks): the update and finish calls of one hash
// object, for the digests that are not the segmented, workspace-bound sha256 of hkdf_sha256.hpp. On Windows the
// functions are inline forwarders to the CNG calls of the same names (windows/hash_stream.inl), so a call site compiles
// to the BCrypt calls it replaced; on Linux they are in src/platform/crypto/openssl/hash_stream.cpp (EVP_MD).
//
// Every function returns a native status: negative is a failure, zero or positive success. Statuses are
// diagnostics only (the Windows value is an NTSTATUS, the Linux one is described in openssl.hpp). A stream is the
// opaque handle of one hash object, null when there is none: begin it, update it, finish it for the digest, and
// destroy it on every path that began it (finish does not release it). Destroy does not clear the owner's handle.

namespace maicord {

enum class HashAlgorithm : u8 {
    sha256, // 32-byte digest
    md5,    // 16-byte digest (a fingerprint the server compares; not for security)
};

using HashStream = void*;

} // namespace maicord

#if defined(_WIN32)
#include "maicord/platform/crypto/windows/hash_stream.inl"
#else
namespace maicord {

i32 hash_stream_begin(HashStream* stream, HashAlgorithm algorithm) noexcept;
i32 hash_stream_update(HashStream stream, const void* data, u32 bytes) noexcept;
// Writes the digest to `digest`; digest_bytes must be exactly the algorithm's digest size (CNG insists on it).
i32 hash_stream_finish(HashStream stream, u8* digest, u32 digest_bytes) noexcept;
i32 hash_stream_destroy(HashStream stream) noexcept;

} // namespace maicord
#endif
