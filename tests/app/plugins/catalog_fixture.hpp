#pragma once
// Shared by the catalog tests: files, hex, the scalar-one signing key the format-2 vectors use, and a
// small text builder for index documents.
#include "maicord/app/plugins/catalog_index.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../support/test_output.hpp"
#include "plugin_fixture.hpp"

namespace maicord::catalog_fixture {

inline u8* read_file(const wchar_t* path, u32* bytes) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    u8* data = nullptr;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024 * 1024) {
        data = static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(size.QuadPart) + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD read = 0;
        if (data && (!ReadFile(file, data, static_cast<DWORD>(size.QuadPart), &read, nullptr) || read != size.QuadPart)) {
            VirtualFree(data, 0, MEM_RELEASE);
            data = nullptr;
        }
        *bytes = static_cast<u32>(size.QuadPart);
    }
    CloseHandle(file);
    return data;
}
inline bool write_file(const wchar_t* path, const void* data, u32 bytes) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    return ok;
}
inline void release(void* memory) noexcept {
    if (memory) VirtualFree(memory, 0, MEM_RELEASE);
}
inline u32 wide_copy(wchar_t* out, u32 at, const wchar_t* text) noexcept {
    while (*text) out[at++] = *text++;
    out[at] = 0;
    return at;
}
using plugin_fixture::exists;
using plugin_fixture::remove_tree;
using plugin_fixture::test_directory;

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
