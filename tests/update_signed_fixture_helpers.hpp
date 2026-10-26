#pragma once
// Bounded, test-only publisher/package helpers. No key or origin here
// authorizes production. The public point is the fixture scalar-one signer.
#include "maicord/app/update_service.hpp"
#include "test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crypto.hpp"

namespace maicord::signed_update_fixture {
// Stream the complete shipping package with fewer fixture-only I/O calls.
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
// Generated, never-executed AMD64 image of kNativeImageBytes: one .text section
// holding a two-instruction entry point.
inline constexpr u32 kNativeImageBytes = 1024;
inline u32 native_image(u8* out, bool dll) noexcept {
    SecureZeroMemory(out, kNativeImageBytes);
    put16(out, 0x5a4d); put32(out + 60, 128);
    u8* nt = out + 128; put32(nt, 0x00004550); put16(nt + 4, 0x8664);
    put16(nt + 6, 1); put16(nt + 20, 240); put16(nt + 22, dll ? 0x2022 : 0x22);
    u8* opt = nt + 24; put16(opt, 0x20b); opt[2] = 14;
    put32(opt + 4, 512); put32(opt + 16, 4096); put32(opt + 20, 4096);
    put32(opt + 24, 0x400000); put32(opt + 32, 4096); put32(opt + 36, 512);
    put16(opt + 40, 6); put16(opt + 48, 6); put32(opt + 56, 8192);
    put32(opt + 60, 512); put16(opt + 68, 3); put16(opt + 70, 0x100);
    put32(opt + 72, 1048576); put32(opt + 80, 4096); put32(opt + 88, 1048576); put32(opt + 96, 4096);
    put32(opt + 108, 16);
    u8* sec = nt + 264; copy_bytes(sec, ".text", 5); put32(sec + 8, 6); put32(sec + 12, 4096);
    put32(sec + 16, 512); put32(sec + 20, 512); put32(sec + 36, 0x60000020);
    out[512] = 0xb8; out[513] = 1; out[517] = 0xc3;
    return kNativeImageBytes;
}
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
// Stored regular members names[i] from paths[i], at most kFixtureMaximumMembers
// (a release package has exactly one, MaiCord.exe). All failed file/CNG owners
// stay explicit.
inline constexpr u32 kFixtureMaximumMembers = 2;
inline bool package(const wchar_t* destination, const wchar_t* const* paths, const char* const* names, u32 count, HANDLE* output,
    HANDLE* sources, u64* package_bytes, u8* digest, BCRYPT_HASH_HANDLE* hash_owner) noexcept {
    if (!count || count > kFixtureMaximumMembers) return false;
    u32 sizes[kFixtureMaximumMembers]{}, checksums[kFixtureMaximumMembers]{}, offsets[kFixtureMaximumMembers]{},
        counts[kFixtureMaximumMembers]{};
    u8 buffer[kFixtureIoBufferBytes]{};
    for (u32 i = 0; i < count; ++i) {
        while (names[i][counts[i]]) ++counts[i];
        sources[i] = CreateFileW(paths[i], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (sources[i] == INVALID_HANDLE_VALUE) { sources[i] = nullptr; return false; }
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(sources[i], &info) || !GetFileSizeEx(sources[i], &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024 ||
            info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        sizes[i] = static_cast<u32>(size.QuadPart); u32 remaining = sizes[i], checksum = ~u32{0};
        while (remaining) { DWORD got = 0; const DWORD wanted = minimum(remaining, static_cast<u32>(sizeof(buffer)));
            if (!ReadFile(sources[i], buffer, wanted, &got, nullptr) || !got || got > wanted) return false;
            checksum = crc(checksum, buffer, got); remaining -= got; } checksums[i] = checksum ^ ~u32{0};
    }
    *output = CreateFileW(destination, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (*output == INVALID_HANDLE_VALUE) { *output = nullptr; return false; }
    u32 next = 0;
    for (u32 i = 0; i < count; ++i) { offsets[i] = next; u8 local[30]{}; put32(local, 0x04034b50); put16(local + 4, 20);
        put32(local + 14, checksums[i]); put32(local + 18, sizes[i]); put32(local + 22, sizes[i]); put16(local + 26, static_cast<u16>(counts[i]));
        if (!write(*output, local, sizeof(local)) || !write(*output, names[i], counts[i]) || !copy(sources[i], *output, sizes[i])) return false;
        next += 30 + counts[i] + sizes[i]; }
    const u32 central = next;
    for (u32 i = 0; i < count; ++i) { u8 entry[46]{}; put32(entry, 0x02014b50); put16(entry + 4, 20); put16(entry + 6, 20);
        put32(entry + 16, checksums[i]); put32(entry + 20, sizes[i]); put32(entry + 24, sizes[i]); put16(entry + 28, static_cast<u16>(counts[i])); put32(entry + 42, offsets[i]);
        if (!write(*output, entry, sizeof(entry)) || !write(*output, names[i], counts[i])) return false; next += 46 + counts[i]; }
    u8 end[22]{}; put32(end, 0x06054b50); put16(end + 8, static_cast<u16>(count)); put16(end + 10, static_cast<u16>(count));
    put32(end + 12, next - central); put32(end + 16, central);
    if (!write(*output, end, sizeof(end)) || !FlushFileBuffers(*output)) return false; *package_bytes = next + sizeof(end);
    return hash_file(*output, *package_bytes, digest, hash_owner);
}
inline u32 decimal(char* out, u64 value) noexcept {
    char reverse[20]{}; u32 n = 0; do { reverse[n++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    for (u32 i = 0; i < n; ++i) out[i] = reverse[n - i - 1]; return n;
}
inline Utf8View manifest(char* out, u64 bytes, const u8* digest, u64 version) noexcept {
    u32 n = 0; auto add = [&](Utf8View text) noexcept { copy_bytes(out + n, text.data, text.bytes); n += text.bytes; };
    add(utf8_literal(R"({"format":)")); n += decimal(out + n, kUpdateProtocol);
    add(utf8_literal(R"(,"product":"MaiCord","arch":"x64","channel":"stable","version":)")); n += decimal(out + n, version);
    add(utf8_literal(R"(,"issued":90,"expires":200,"bytes":)")); n += decimal(out + n, bytes); add(utf8_literal(R"(,"sha256":")")); constexpr char digits[] = "0123456789abcdef";
    for (u32 i = 0; i < 32; ++i) { out[n++] = digits[digest[i] >> 4]; out[n++] = digits[digest[i] & 15]; }
    add(utf8_literal(R"(","artifact":")")); add(kUpdateArtifactPrefix);
    n += decimal(out + n, version); add(utf8_literal(R"(.zip"})")); return {out,n};
}
inline bool sign(Utf8View document, const P256Key& signer, KdfSha256Workspace& workspace, u8* signature) noexcept {
    u8 digest[32]{}; const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(document.data), document.bytes}};
    return sha256(workspace, {parts,2}, {digest,32}).ok() && p256_sign_digest(signer, {digest,32}, {signature,64}).ok();
}
} // namespace maicord::signed_update_fixture
