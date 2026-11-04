#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/platform/crypto/hash_stream.hpp"

namespace maicord {

// Lowercase hex MD5 of an original image file, as the official client reports it in
// X-Discord-Original-MD5 when it uploads an image; false when it can't be hashed.
inline bool original_md5_hex(ConstSlice<u8> data, char (&hex)[32]) noexcept {
    HashStream hash = nullptr;
    u8 digest[16]{};
    bool ok = hash_stream_begin(&hash, HashAlgorithm::md5) >= 0;
    for (u32 at = 0; ok && at < data.count;) {
        const u32 chunk = data.count - at > 0x10000000u ? 0x10000000u : data.count - at;
        ok = hash_stream_update(hash, data.data + at, chunk) >= 0;
        at += chunk;
    }
    ok = ok && hash_stream_finish(hash, digest, sizeof(digest)) >= 0;
    if (hash)
        hash_stream_destroy(hash);
    if (ok)
        for (u32 i = 0; i < 16; ++i) {
            hex[i * 2] = "0123456789abcdef"[digest[i] >> 4];
            hex[i * 2 + 1] = "0123456789abcdef"[digest[i] & 15];
        }
    return ok;
}

} // namespace maicord
