#pragma once
// Shared by the catalog tests on both systems (catalog_fixture.hpp adds the Windows file helpers): hex, a small text
// builder for index documents and the P-256 key whose scalar is 1, which the format-2 vectors and the test catalogs
// sign with.
#include "maicord/app/plugins/catalog_index.hpp"

namespace maicord::catalog_fixture {

inline void hex(const u8* bytes, u32 count, char* out) noexcept {
    static constexpr char digits[] = "0123456789abcdef";
    for (u32 i = 0; i < count; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
}

// A growing text in one fixed buffer.
struct Text {
    char data[256 * 1024];
    u32 bytes;
    Text& add(const char* s) noexcept {
        while (*s && bytes < sizeof(data)) data[bytes++] = *s++;
        return *this;
    }
    Text& add(Utf8View s) noexcept {
        for (u32 i = 0; i < s.bytes && bytes < sizeof(data); ++i) data[bytes++] = s.data[i];
        return *this;
    }
    Text& number(u64 value) noexcept {
        char digits[24];
        u32 n = 0;
        do digits[n++] = static_cast<char>('0' + value % 10), value /= 10;
        while (value);
        while (n && bytes < sizeof(data)) data[bytes++] = digits[--n];
        return *this;
    }
    Text& digest(const u8 (&sha256)[32]) noexcept {
        if (bytes + 64 <= sizeof(data)) hex(sha256, 32, data + bytes), bytes += 64;
        return *this;
    }
    Utf8View view() const noexcept { return {data, bytes}; }
};

// The P-256 key whose scalar is 1: the vectors' signing key.
struct SigningKey {
    P256Key private_key, public_key;
    KdfSha256Workspace workspace;
    bool ready;
    bool open() noexcept {
        u8 scalar[32]{};
        scalar[31] = 1;
        u8 point[65]{};
        ready = p256_import_private(&private_key, P256Kind::ecdsa, {scalar, 32}).ok() &&
                p256_export_public(private_key, {point, 65}).ok() &&
                p256_import_public(&public_key, P256Kind::ecdsa, {point, 65}).ok() && kdf_sha256_initialize(&workspace).ok();
        return ready;
    }
    void close() noexcept {
        p256_release(&private_key);
        p256_release(&public_key);
        if (ready) kdf_sha256_release(&workspace);
        ready = false;
    }
    bool point(u8 (&out)[65]) const noexcept { return p256_export_public(public_key, {out, 65}).ok(); }
    bool digest(ConstSlice<u8> bytes, u8 (&out)[32]) noexcept {
        const ConstSlice<u8> parts[]{bytes};
        return sha256(workspace, {parts, 1}, {out, 32}).ok();
    }
    bool sign(Utf8View document, u8 (&signature)[64]) noexcept {
        const ConstSlice<u8> parts[]{
            {reinterpret_cast<const u8*>(kCatalogIndexSignatureDomain.data), kCatalogIndexSignatureDomain.bytes},
            {reinterpret_cast<const u8*>(document.data), document.bytes}};
        u8 digest[32];
        return sha256(workspace, {parts, 2}, {digest, 32}).ok() && p256_sign_digest(private_key, {digest, 32}, {signature, 64}).ok();
    }
};

} // namespace maicord::catalog_fixture
