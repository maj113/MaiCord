#include "maicord/platform/crypto/hash_stream.hpp"
#include "maicord/core/memory.hpp"
#include "../../support/test_output.hpp"
#include "../../support/test_sha256.hpp"

// The incremental SHA-256 and MD5 of hash_stream.hpp: published digests, chunk-size independence and the lifecycle.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool equals_hex(const u8* actual, const char* hex) noexcept {
    u8 expected[32];
    const u32 bytes = test_unhex(hex, expected);
    return bytes_equal(actual, expected, bytes);
}

// Hashes `data` in chunks of `chunk` bytes; false when any call failed.
bool run(HashAlgorithm algorithm, const u8* data, u32 bytes, u32 chunk, u8* digest, u32 digest_bytes) noexcept {
    HashStream stream = nullptr;
    if (hash_stream_begin(&stream, algorithm) < 0 || !stream)
        return false;
    bool good = true;
    for (u32 at = 0; good && at < bytes; at += chunk)
        good = hash_stream_update(stream, data + at, bytes - at < chunk ? bytes - at : chunk) >= 0;
    good = good && hash_stream_finish(stream, digest, digest_bytes) >= 0;
    hash_stream_destroy(stream);
    return good;
}

} // namespace

void test_hash_stream(Check check) noexcept {
    u8 digest[40];
    memset(digest, 0xa5, sizeof(digest));
    check(run(HashAlgorithm::sha256, nullptr, 0, 1, digest, 32) &&
              equals_hex(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "SHA-256 stream of nothing");
    const u8 abc[]{'a', 'b', 'c'};
    check(run(HashAlgorithm::sha256, abc, 3, 3, digest, 32) &&
              equals_hex(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256 stream of abc");
    check(run(HashAlgorithm::md5, abc, 3, 1, digest, 16) && equals_hex(digest, "900150983cd24fb0d6963f7d28e17f72"),
          "MD5 stream of abc");
    check(run(HashAlgorithm::md5, nullptr, 0, 1, digest, 16) && equals_hex(digest, "d41d8cd98f00b204e9800998ecf8427e"),
          "MD5 stream of nothing");

    // Any chunking of 100,003 bytes gives the digest of an independent implementation.
    static u8 data[100003];
    for (u32 i = 0; i != sizeof(data); ++i)
        data[i] = static_cast<u8>(i * 31 + (i >> 7));
    TestSha256 reference;
    test_sha256_begin(&reference);
    test_sha256_update(&reference, data, sizeof(data));
    u8 expected[32];
    test_sha256_finish(&reference, expected);
    constexpr u32 chunks[]{1, 63, 64, 65, 4096, 8192, 100003};
    bool chunking = true;
    for (u32 chunk : chunks)
        chunking = chunking && run(HashAlgorithm::sha256, data, sizeof(data), chunk, digest, 32) && bytes_equal(digest, expected, 32);
    check(chunking, "SHA-256 stream digests do not depend on the chunking");

    // Lifecycle: the digest is written into the front of a larger buffer and its tail is kept.
    memset(digest, 0xa5, sizeof(digest));
    HashStream stream = nullptr;
    check(hash_stream_begin(&stream, HashAlgorithm::sha256) >= 0 && hash_stream_update(stream, abc, 3) >= 0 &&
              hash_stream_finish(stream, digest, 32) >= 0 &&
              equals_hex(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") &&
              test_filled(digest + 32, 8, 0xa5),
          "a stream finishes into the front of a larger buffer and keeps its tail");
    check(hash_stream_destroy(stream) >= 0, "a stream is destroyed");
}

} // namespace maicord
