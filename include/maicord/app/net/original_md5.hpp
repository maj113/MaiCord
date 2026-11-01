#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/sys/crypto.hpp"

namespace maicord {

// Lowercase hex MD5 of an original image file, as the official client reports it in
// X-Discord-Original-MD5 when it uploads an image; false when it can't be hashed.
inline bool original_md5_hex(ConstSlice<u8> data, char (&hex)[32]) noexcept {
    BCRYPT_HASH_HANDLE hash = nullptr;
    u8 digest[16]{};
    bool ok = BCryptCreateHash(BCRYPT_MD5_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    for (u32 at = 0; ok && at < data.count;) {
        const u32 chunk = data.count - at > 0x10000000u ? 0x10000000u : data.count - at;
        ok = BCryptHashData(hash, const_cast<u8*>(data.data + at), chunk, 0) >= 0;
        at += chunk;
    }
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (ok)
        for (u32 i = 0; i < 16; ++i) {
            hex[i * 2] = "0123456789abcdef"[digest[i] >> 4];
            hex[i * 2 + 1] = "0123456789abcdef"[digest[i] & 15];
        }
    return ok;
}

} // namespace maicord
