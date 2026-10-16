#pragma once
// Bounded, test-only publisher/install helpers. No key/origin/enrollment here
// authorizes production. The public point is the fixture scalar-one signer.
#include "maicord/app/update_install.hpp"
#include "test_output.hpp"
#include <bcrypt.h>

namespace maicord::signed_update_fixture {
// Generated, never-executed native pair ZIP. Defined once beside the parser's
// corruption fixtures in update_pair_package.cpp; requires 4096 output bytes.
u32 native_pair_package(Slice<u8> output) noexcept;
// Stream the complete shipping pair with fewer fixture-only I/O calls.
inline constexpr u32 kFixtureIoBufferBytes = 64 * 1024;
inline constexpr u8 public_point[65]{
    4, 0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96,
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
inline u32 length(const wchar_t* s) noexcept { u32 n = 0; while (s[n]) ++n; return n; }
inline void child(const wchar_t* root, const wchar_t* name, wchar_t* out) noexcept {
    u32 n = 0; while (root[n]) { out[n] = root[n]; ++n; } out[n++] = L'\\';
    for (u32 i = 0;; ++i) { out[n + i] = name[i]; if (!name[i]) return; }
}
inline bool dispose(HANDLE* h) noexcept {
    if (!*h) return true; FILE_DISPOSITION_INFO info{TRUE};
    if (!SetFileInformationByHandle(*h, FileDispositionInfo, &info, sizeof(info)) || !CloseHandle(*h)) return false;
    *h = nullptr; return true;
}
inline bool close(HANDLE* h) noexcept { if (!*h) return true; if (!CloseHandle(*h)) return false; *h = nullptr; return true; }
inline bool write(HANDLE h, const void* data, u32 bytes) noexcept {
    u32 done = 0;
    while (done < bytes) { DWORD n = 0; if (!WriteFile(h, static_cast<const u8*>(data) + done, bytes - done, &n, nullptr) || !n || n > bytes - done) return false; done += n; }
    return true;
}
inline bool seek(HANDLE h) noexcept { LARGE_INTEGER zero{}; return SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) != FALSE; }
inline bool copy(HANDLE source, HANDLE destination, u64 bytes) noexcept {
    if (!seek(source)) return false; u8 buffer[kFixtureIoBufferBytes]{};
    while (bytes) { DWORD got = 0; const DWORD wanted = static_cast<DWORD>(minimum<u64>(bytes, sizeof(buffer)));
        if (!ReadFile(source, buffer, wanted, &got, nullptr) || !got || got > wanted || !write(destination, buffer, got)) return false; bytes -= got; }
    return true;
}
inline void put16(u8* p, u16 v) noexcept { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
inline void put32(u8* p, u32 v) noexcept { put16(p, static_cast<u16>(v)); put16(p + 2, static_cast<u16>(v >> 16)); }
inline u32 crc(u32 value, const u8* p, u32 bytes) noexcept {
    // Byte-table IEEE CRC-32; identical to the bitwise definition, which
    // dominated these multi-megabyte package fixtures.
    static constexpr auto table = [] { struct T { u32 v[256]; } t{};
        for (u32 i = 0; i < 256; ++i) { u32 c = i; for (u32 bit = 0; bit < 8; ++bit) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); t.v[i] = c; }
        return t; }();
    for (u32 i = 0; i < bytes; ++i) value = (value >> 8) ^ table.v[(value ^ p[i]) & 255];
    return value;
}
inline bool hash_file(HANDLE h, u64 bytes, u8* digest, BCRYPT_HASH_HANDLE* owner) noexcept {
    if (*owner || !seek(h) || BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, owner, nullptr, 0, nullptr, 0, 0) < 0) return false;
    u8 buffer[kFixtureIoBufferBytes]{}; bool ok = true;
    while (ok && bytes) { DWORD got = 0; const DWORD wanted = static_cast<DWORD>(minimum<u64>(bytes, sizeof(buffer)));
        ok = ReadFile(h, buffer, wanted, &got, nullptr) && got && got <= wanted && BCryptHashData(*owner, buffer, got, 0) >= 0;
        if (got <= bytes) bytes -= got; else ok = false; }
    if (ok) ok = BCryptFinishHash(*owner, digest, 32, 0) >= 0;
    if (BCryptDestroyHash(*owner) < 0) return false; *owner = nullptr; return ok;
}
// Exactly two stored regular members. All failed file/CNG owners stay explicit.
inline bool package(const wchar_t* destination, const wchar_t* const* paths, HANDLE* output, HANDLE* sources,
    u64* package_bytes, u8* digest, BCRYPT_HASH_HANDLE* hash_owner) noexcept {
    u32 sizes[2]{}, checksums[2]{}, offsets[2]{}; u8 buffer[kFixtureIoBufferBytes]{};
    for (u32 i = 0; i < 2; ++i) {
        sources[i] = CreateFileW(paths[i], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (sources[i] == INVALID_HANDLE_VALUE) { sources[i] = nullptr; return false; }
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(sources[i], &info) || !GetFileSizeEx(sources[i], &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024 ||
            info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || info.nNumberOfLinks != 1) return false;
        sizes[i] = static_cast<u32>(size.QuadPart); u32 remaining = sizes[i], checksum = ~u32{0};
        while (remaining) { DWORD got = 0; const DWORD wanted = minimum(remaining, static_cast<u32>(sizeof(buffer)));
            if (!ReadFile(sources[i], buffer, wanted, &got, nullptr) || !got || got > wanted) return false;
            checksum = crc(checksum, buffer, got); remaining -= got; } checksums[i] = checksum ^ ~u32{0};
    }
    *output = CreateFileW(destination, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (*output == INVALID_HANDLE_VALUE) { *output = nullptr; return false; }
    constexpr char names[2][19]{"MaiCord.exe", "MaiCordAccount.dll"}; constexpr u32 counts[]{11,18}; u32 next = 0;
    for (u32 i = 0; i < 2; ++i) { offsets[i] = next; u8 local[30]{}; put32(local, 0x04034b50); put16(local + 4, 20);
        put32(local + 14, checksums[i]); put32(local + 18, sizes[i]); put32(local + 22, sizes[i]); put16(local + 26, static_cast<u16>(counts[i]));
        if (!write(*output, local, sizeof(local)) || !write(*output, names[i], counts[i]) || !copy(sources[i], *output, sizes[i])) return false;
        next += 30 + counts[i] + sizes[i]; }
    const u32 central = next;
    for (u32 i = 0; i < 2; ++i) { u8 entry[46]{}; put32(entry, 0x02014b50); put16(entry + 4, 20); put16(entry + 6, 20);
        put32(entry + 16, checksums[i]); put32(entry + 20, sizes[i]); put32(entry + 24, sizes[i]); put16(entry + 28, static_cast<u16>(counts[i])); put32(entry + 42, offsets[i]);
        if (!write(*output, entry, sizeof(entry)) || !write(*output, names[i], counts[i])) return false; next += 46 + counts[i]; }
    u8 end[22]{}; put32(end, 0x06054b50); put16(end + 8, 2); put16(end + 10, 2); put32(end + 12, next - central); put32(end + 16, central);
    if (!write(*output, end, sizeof(end)) || !FlushFileBuffers(*output)) return false; *package_bytes = next + sizeof(end);
    return hash_file(*output, *package_bytes, digest, hash_owner);
}
// Publisher-fixture ZIP producer: streams the actual built shipping EXE/DLL in
// fixed order. No DLL execution, mmap, archive library, compression or heap.
inline bool real_package(const wchar_t* path, HANDLE* package_owner, HANDLE* sources, u64* package_bytes, u8* digest) noexcept {
#if defined(MAICORD_UPDATE_PAIR_TEST_EXECUTABLE) && defined(MAICORD_UPDATE_PAIR_TEST_ACCOUNT)
    const wchar_t* paths[]{MAICORD_UPDATE_PAIR_TEST_EXECUTABLE, MAICORD_UPDATE_PAIR_TEST_ACCOUNT};
#else
    (void)path; (void)package_owner; (void)sources; (void)package_bytes; (void)digest;
    return false; // Root must supply explicit built shipping fixture paths.
#endif
#if defined(MAICORD_UPDATE_PAIR_TEST_EXECUTABLE) && defined(MAICORD_UPDATE_PAIR_TEST_ACCOUNT)
    u32 sizes[2]{}, crcs[2]{}, offsets[2]{}; u8 buffer[kFixtureIoBufferBytes]{};
    for (u32 i = 0; i < 2; ++i) {
        sources[i] = CreateFileW(paths[i], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (sources[i] == INVALID_HANDLE_VALUE) { sources[i] = nullptr; return false; }
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(sources[i], &info) || !GetFileSizeEx(sources[i], &size) || size.QuadPart <= 0 ||
            size.QuadPart > 64 * 1024 * 1024 || info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        sizes[i] = static_cast<u32>(size.QuadPart); u32 remaining = sizes[i], checksum = 0xffffffff;
        while (remaining) { DWORD got = 0; const DWORD wanted = minimum(remaining, static_cast<u32>(sizeof(buffer)));
            if (!ReadFile(sources[i], buffer, wanted, &got, nullptr) || !got || got > wanted) return false;
            checksum = crc(checksum, buffer, got); remaining -= got; } crcs[i] = checksum ^ 0xffffffff;
    }
    *package_owner = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (*package_owner == INVALID_HANDLE_VALUE) { *package_owner = nullptr; return false; }
    constexpr char names[2][19]{"MaiCord.exe", "MaiCordAccount.dll"}; constexpr u32 counts[2]{11, 18};
    u32 next = 0;
    for (u32 i = 0; i < 2; ++i) {
        offsets[i] = next; u8 local[30]{}; put32(local, 0x04034b50); put16(local + 4, 20);
        put32(local + 14, crcs[i]); put32(local + 18, sizes[i]); put32(local + 22, sizes[i]); put16(local + 26, static_cast<u16>(counts[i]));
        if (!write(*package_owner, local, sizeof(local)) || !write(*package_owner, names[i], counts[i]) ||
            !copy(sources[i], *package_owner, sizes[i])) return false; next += 30 + counts[i] + sizes[i];
    }
    const u32 central = next;
    for (u32 i = 0; i < 2; ++i) {
        u8 entry[46]{}; put32(entry, 0x02014b50); put16(entry + 4, 20); put16(entry + 6, 20);
        put32(entry + 16, crcs[i]); put32(entry + 20, sizes[i]); put32(entry + 24, sizes[i]); put16(entry + 28, static_cast<u16>(counts[i]));
        put32(entry + 42, offsets[i]); if (!write(*package_owner, entry, sizeof(entry)) || !write(*package_owner, names[i], counts[i])) return false;
        next += 46 + counts[i];
    }
    u8 end[22]{}; put32(end, 0x06054b50); put16(end + 8, 2); put16(end + 10, 2); put32(end + 12, next - central); put32(end + 16, central);
    if (!write(*package_owner, end, sizeof(end)) || !FlushFileBuffers(*package_owner) || !seek(*package_owner)) return false;
    *package_bytes = next + 22;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    bool ok = true; u64 remaining = *package_bytes;
    while (remaining && ok) { DWORD got = 0; const DWORD wanted = static_cast<DWORD>(minimum<u64>(remaining, sizeof(buffer)));
        ok = ReadFile(*package_owner, buffer, wanted, &got, nullptr) && got && got <= wanted && BCryptHashData(hash, buffer, got, 0) >= 0;
        remaining -= got; }
    if (ok) ok = BCryptFinishHash(hash, digest, 32, 0) >= 0;
    const bool destroyed = BCryptDestroyHash(hash) >= 0;
    return ok && destroyed;
#endif
}
inline bool ancestry(const wchar_t* root, UpdateGenerationDirectoryIdentity* ids, u32* count) noexcept {
    wchar_t path[MAX_PATH]{}; const u32 n = length(root); copy_bytes(path, root, (n + 1) * sizeof(wchar_t)); *count = 0;
    for (u32 i = 2; i <= n; ++i) if (i == 2 || i == n || path[i] == L'\\') { const u32 end = i == 2 ? 3 : i; const wchar_t saved = path[end]; path[end] = 0;
        HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr); path[end] = saved;
        if (h == INVALID_HANDLE_VALUE) return false; BY_HANDLE_FILE_INFORMATION info{};
        const bool ok = GetFileInformationByHandle(h, &info) != FALSE; const bool closed = CloseHandle(h) != FALSE;
        if (!ok || !closed || *count == kUpdateGenerationMaximumAncestors) return false;
        ids[(*count)++] = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow}; }
    return true;
}
inline u32 decimal(char* out, u64 value) noexcept {
    char reverse[20]{}; u32 n = 0; do { reverse[n++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    for (u32 i = 0; i < n; ++i) out[i] = reverse[n - i - 1]; return n;
}
inline Utf8View manifest(char* out, u64 bytes, const u8* digest, u64 version) noexcept {
    u32 n = 0; auto add = [&](Utf8View text) noexcept { copy_bytes(out + n, text.data, text.bytes); n += text.bytes; };
    add(utf8_literal(R"({"format":1,"product":"MaiCord","arch":"x64","channel":"stable","version":)")); n += decimal(out + n, version);
    add(utf8_literal(R"(,"issued":90,"expires":200,"bytes":)")); n += decimal(out + n, bytes); add(utf8_literal(R"(,"sha256":")")); constexpr char digits[] = "0123456789abcdef";
    for (u32 i = 0; i < 32; ++i) { out[n++] = digits[digest[i] >> 4]; out[n++] = digits[digest[i] & 15]; }
    add(utf8_literal(R"(","artifact":"MaiCord-x64-)")); n += decimal(out + n, version); add(utf8_literal(R"(.zip"})")); return {out,n};
}
inline void measurement(const char* name, usize value) noexcept {
    test_write_metric(name, value);
}
inline bool sign(Utf8View document, const P256Key& signer, KdfSha256Workspace& workspace, u8* signature) noexcept {
    u8 digest[32]{}; const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(document.data), document.bytes}};
    return sha256(workspace, {parts,2}, {digest,32}).ok() && p256_sign_digest(signer, {digest,32}, {signature,64}).ok();
}
} // namespace maicord::signed_update_fixture
