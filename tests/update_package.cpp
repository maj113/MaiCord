#include "maicord/app/update_package.hpp"
#include "update_signed_fixture_helpers.hpp"

namespace maicord {
namespace {
using signed_update_fixture::put16;
using signed_update_fixture::put32;
u32 crc32(const u8* data, u32 count) noexcept {
    u32 crc = 0xffffffff;
    for (u32 i = 0; i < count; ++i) {
        crc ^= data[i];
        for (u32 bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return crc ^ 0xffffffff;
}
// Generated PE format fixtures, never executed/loaded. The real shipping
// executable is packaged by the client pipeline tests (tests/update_client.cpp).
constexpr u32 kImageBytes = signed_update_fixture::kNativeImageBytes;
constexpr u32 kNameBytes = 11, kNotesBytes = 64;
// MaiCord.exe (an image with `overlay` trailing bytes) and, for the refused
// two-member layout, a data member after it.
struct ZipLayout { u32 bytes, local, data, central, size, end; };
ZipLayout fixture(u8* out, u32 overlay = 0, bool second = false) noexcept {
    const u32 capacity = 4096 + overlay;
    SecureZeroMemory(out, capacity);
    ZipLayout z{};
    put32(out, 0x04034b50); put16(out + 4, 20); put16(out + 26, kNameBytes);
    copy_bytes(out + 30, "MaiCord.exe", kNameBytes); z.data = 30 + kNameBytes;
    signed_update_fixture::native_image(out + z.data, false);
    for (u32 b = 0; b < overlay; ++b) out[z.data + kImageBytes + b] = static_cast<u8>('a' + b % 26);
    z.size = kImageBytes + overlay;
    const u32 crc = crc32(out + z.data, z.size);
    put32(out + 14, crc); put32(out + 18, z.size); put32(out + 22, z.size);
    u32 offset = z.data + z.size, notes_local = 0, notes_crc = 0;
    if (second) {
        notes_local = offset; u8* h = out + offset;
        put32(h, 0x04034b50); put16(h + 4, 20); put16(h + 26, 9); copy_bytes(h + 30, "notes.txt", 9);
        for (u32 b = 0; b < kNotesBytes; ++b) h[39 + b] = static_cast<u8>('a' + b % 26);
        notes_crc = crc32(h + 39, kNotesBytes);
        put32(h + 14, notes_crc); put32(h + 18, kNotesBytes); put32(h + 22, kNotesBytes);
        offset += 39 + kNotesBytes;
    }
    const u32 central_start = offset;
    z.central = offset; u8* h = out + offset;
    put32(h, 0x02014b50); put16(h + 4, 20); put16(h + 6, 20); put32(h + 16, crc);
    put32(h + 20, z.size); put32(h + 24, z.size); put16(h + 28, kNameBytes);
    copy_bytes(h + 46, "MaiCord.exe", kNameBytes); offset += 46 + kNameBytes;
    if (second) {
        h = out + offset;
        put32(h, 0x02014b50); put16(h + 4, 20); put16(h + 6, 20); put32(h + 16, notes_crc);
        put32(h + 20, kNotesBytes); put32(h + 24, kNotesBytes); put16(h + 28, 9); put32(h + 42, notes_local);
        copy_bytes(h + 46, "notes.txt", 9); offset += 46 + 9;
    }
    const u16 members = second ? 2 : 1;
    z.end = offset; h = out + offset; put32(h, 0x06054b50); put16(h + 8, members); put16(h + 10, members);
    put32(h + 12, offset - central_start); put32(h + 16, central_start); z.bytes = offset + 22; return z;
}
void repair_crc(u8* bytes, const ZipLayout& z) noexcept {
    const u32 crc = crc32(bytes + z.data, z.size);
    put32(bytes + 14, crc); put32(bytes + z.central + 16, crc);
}
// Renames the member in both its local and central headers.
void rename(u8* bytes, const ZipLayout& z, const char* name) noexcept {
    copy_bytes(bytes + 30, name, kNameBytes);
    copy_bytes(bytes + z.central + 46, name, kNameBytes);
}
u32 characters(const wchar_t* text) noexcept { u32 n = 0; while (text[n]) ++n; return n; }
void append(wchar_t* out, const wchar_t* parent, const wchar_t* name) noexcept {
    u32 n = 0; while (parent[n]) { out[n] = parent[n]; ++n; }
    out[n++] = L'\\'; u32 j = 0; do { out[n + j] = name[j]; } while (name[j++]);
}
bool write(const wchar_t* path, const u8* bytes, u32 count) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; const bool ok = WriteFile(file, bytes, count, &written, nullptr) && written == count;
    return CloseHandle(file) && ok;
}
bool present(const wchar_t* path) noexcept { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
// The file at path holds exactly count bytes equal to expected. A held
// stage's file object keeps its creating write access, so a reader must
// share writing as well.
bool holds(const wchar_t* path, const u8* expected, u32 count) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    u8 buffer[1024]{}; bool ok = true; u32 offset = 0;
    while (ok) {
        DWORD got = 0;
        ok = ReadFile(file, buffer, sizeof(buffer), &got, nullptr) != FALSE;
        if (!ok || !got) break;
        ok = offset + got <= count && bytes_equal(buffer, expected + offset, got);
        offset += got;
    }
    return CloseHandle(file) && ok && offset == count;
}
} // namespace

void test_update_package(void (*check)(bool, const char*) noexcept) noexcept {
    wchar_t temporary[MAX_PATH]{}, directory[MAX_PATH]{}, artifact[MAX_PATH]{}, acceptance_path[MAX_PATH]{}, lock_path[MAX_PATH]{};
    wchar_t staged[MAX_PATH]{}, missing_parent[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, temporary);
    if (!n || n >= MAX_PATH || !GetTempFileNameW(temporary, L"MPP", 0, directory)) { check(false, "package: owned isolated fixture path"); return; }
    if (!DeleteFileW(directory) || !CreateDirectoryW(directory, nullptr)) { check(false, "package: owned fixture directory"); return; }
    append(artifact, directory, L"artifact.zip"); append(acceptance_path, directory, L"acceptance.bin");
    append(staged, directory, L"MaiCord.exe.update"); append(missing_parent, directory, L"missing\\MaiCord.exe.update");
    u32 lock_count = characters(acceptance_path); copy_bytes(lock_path, acceptance_path, lock_count * sizeof(wchar_t));
    copy_bytes(lock_path + lock_count, L".lock", 6 * sizeof(wchar_t));
    KdfSha256Workspace workspace{}; P256Key signer{}, key{}; UpdatePreparationContext preparation{};
    UpdatePackage package{}; UpdatePackageResult extracted{};
    struct Cleanup {
        KdfSha256Workspace& workspace; P256Key& signer; P256Key& key; UpdatePreparationContext& preparation; UpdatePackage& package;
        const wchar_t* directory; const wchar_t* artifact; const wchar_t* acceptance; const wchar_t* lock; const wchar_t* staged;
        ~Cleanup() noexcept {
            update_package_release(&package, true); update_preparation_release(&preparation);
            p256_release(&key); p256_release(&signer); kdf_sha256_release(&workspace);
            DeleteFileW(staged); DeleteFileW(artifact); DeleteFileW(acceptance); DeleteFileW(lock); RemoveDirectoryW(directory);
        }
    } cleanup{workspace, signer, key, preparation, package, directory, artifact, acceptance_path, lock_path, staged};
    u8 scalar[32]{}; scalar[31] = 1; u8 point[65]{}, signature[64]{}, digest[32]{};
    const bool crypto = kdf_sha256_initialize(&workspace).ok() && p256_import_private(&signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(signer, {point, 65}).ok() && p256_import_public(&key, P256Kind::ecdsa, {point, 65}).ok();
    check(crypto, "package: independent fixture signing/trusted key initializes"); if (!crypto) return;
    u8 storage[4096]{}; u8* bytes = storage; char text[512]{}; u32 scratch[256]{}; u64 version = 6;
    UpdatePackageOffer offer{}; UpdateManifestPolicy policy{4, 0, 100, 65536, 1000, UpdateChannel::stable};
    ConstSlice<wchar_t> destination{staged, characters(staged)};
    // Signs the next version over bytes[0, count) and admits it: durable
    // acceptance plus the verified artifact, as the service does.
    auto admit = [&](u32 count) noexcept {
        if (!update_package_release(&package, true).ok() || !update_preparation_release(&preparation).ok()) return false;
        const ConstSlice<u8> package_parts[]{{bytes, count}};
        if (!sha256(workspace, {package_parts, 1}, {digest, 32}).ok()) return false;
        offer.document = signed_update_fixture::manifest(text, count, digest, version++);
        offer.signature = {signature, 64};
        if (!signed_update_fixture::sign(offer.document, signer, workspace, signature) || !write(artifact, bytes, count)) return false;
        const UpdatePreparationRequest request{{acceptance_path, characters(acceptance_path)}, {artifact, characters(artifact)},
            offer.document, offer.signature, true};
        return update_prepare_artifact(&preparation, request, key, workspace, &policy, {scratch, 256}).ok();
    };
    auto extract = [&]() noexcept {
        extracted = update_package_extract(&preparation.artifact, offer, key, workspace, policy, preparation.acceptance,
            {scratch, 256}, destination, &package);
        return extracted.ok();
    };
    const auto z = fixture(bytes);
    check(admit(z.bytes) && extract() && package.file && package.version == 6 && package.bytes == kImageBytes &&
        holds(staged, bytes + z.data, kImageBytes),
        "package: a signed stored ZIP of MaiCord.exe extracts the verified executable to the stage path");
    if (!package.file) return;
    DWORD written = 0; const u8 changed = 1;
    check(!WriteFile(package.file, &changed, 1, &written, nullptr) && GetLastError() == ERROR_ACCESS_DENIED,
        "package: the staged executable's handle cannot write");
    HANDLE writer = CreateFileW(staged, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD writer_error = writer == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    check(writer == INVALID_HANDLE_VALUE && writer_error == ERROR_SHARING_VIOLATION && !DeleteFileW(staged) && present(staged),
        "package: while held, the stage denies other writers and deletion");
    const auto saved = package;
    check(update_package_extract(&preparation.artifact, offer, key, workspace, policy, preparation.acceptance,
        {reinterpret_cast<u32*>(&package), 8}, destination, &package).error == UpdatePackageError::invalid_input &&
        bytes_equal(&saved, &package, sizeof(package)), "package: alias and already-owned output refuse mutation");
    check(update_package_release(&package, false).ok() && !package.file && present(staged),
        "package: a preserving release keeps the staged file");
    check(!extract() && extracted.error == UpdatePackageError::create && extracted.native_error == ERROR_FILE_EXISTS &&
        !package.file && holds(staged, bytes + z.data, kImageBytes),
        "package: an existing stage is never overwritten or deleted by a new extraction");
    check(DeleteFileW(staged) && extract() && package.file, "package: the same admitted offer extracts again once the path is free");
    check(update_package_release(&package, true).ok() && !package.file && !present(staged),
        "package: a discarding release deletes the stage through its handle");
    // No handle or hash supplied by a caller can authenticate different bytes.
    wchar_t damaged_path[MAX_PATH]{}; append(damaged_path, directory, L"damaged.zip");
    bytes[z.data + 600] ^= 1;
    HANDLE damaged = CreateFileW(damaged_path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (damaged != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        const bool written_ok = WriteFile(damaged, bytes, z.bytes, &wrote, nullptr) && wrote == z.bytes;
        UpdateArtifact forged = preparation.artifact; forged.file = damaged;
        const auto refused = update_package_extract(&forged, offer, key, workspace, policy, preparation.acceptance,
            {scratch, 256}, destination, &package);
        check(written_ok && refused.error == UpdatePackageError::digest && !package.file && !present(staged),
            "package: copied caller hashes and a foreign handle cannot authenticate modified package bytes");
        FILE_DISPOSITION_INFO disposition{TRUE};
        check(SetFileInformationByHandle(damaged, FileDispositionInfo, &disposition, sizeof(disposition)) && CloseHandle(damaged),
            "package: damaged fixture source disposed by its handle");
    } else check(false, "package: damaged source fixture CREATE_NEW");
    fixture(bytes);
    UpdateArtifact short_source = preparation.artifact; --short_source.bytes;
    const auto short_result = update_package_extract(&short_source, offer, key, workspace, policy, preparation.acceptance,
        {scratch, 256}, destination, &package);
    UpdateArtifact other_version = preparation.artifact; ++other_version.version;
    const auto version_result = update_package_extract(&other_version, offer, key, workspace, policy, preparation.acceptance,
        {scratch, 256}, destination, &package);
    check(short_result.error == UpdatePackageError::source && version_result.error == UpdatePackageError::source && !present(staged),
        "package: artifact metadata that differs from the signed offer is refused");
    signature[0] ^= 1;
    check(!extract() && extracted.error == UpdatePackageError::offer && !present(staged),
        "package: a changed signature refuses before creating the stage"); signature[0] ^= 1;
    UpdateManifestPolicy stale_policy = policy; stale_policy.now_unix_seconds = 200;
    const auto stale = update_package_extract(&preparation.artifact, offer, key, workspace, stale_policy, preparation.acceptance,
        {scratch, 256}, destination, &package);
    check(stale.error == UpdatePackageError::offer && stale.manifest_error == UpdateManifestError::time && !present(staged),
        "package: an expired offer is refused at extraction");
    destination = {missing_parent, characters(missing_parent)};
    check(!extract() && extracted.error == UpdatePackageError::create && !package.file,
        "package: a stage path that cannot be created fails without output");
    destination = {staged, characters(staged)};
    for (u32 mutation = 0; mutation < 22; ++mutation) {
        auto layout = fixture(bytes, 0, mutation == 21);
        u32 count = layout.bytes; UpdatePackageError expected = UpdatePackageError::zip;
        switch (mutation) {
        case 0: put16(bytes + 6, 1); break; // encryption
        case 1: put16(bytes + 6, 8); break; // data descriptor
        case 2: put16(bytes + 8, 8); break; // deflate
        case 3: put16(bytes + 28, 1); break; // extra field
        case 4: put16(bytes + layout.central + 32, 1); break; // comment
        case 5: bytes[count++] = 0; break; // appended byte
        case 6: --count; break; // truncated end
        case 7: put32(bytes + layout.central + 42, 1); break; // local header elsewhere
        case 8: bytes[30] = 'm'; break; // local/central name mismatch
        case 9: rename(bytes, layout, "maicord.exe"); break; // the name is exact
        case 10: rename(bytes, layout, "MaiCord.dll"); break;
        case 11: put32(bytes + 18, 0xffffffff); put32(bytes + 22, 0xffffffff); break; // ZIP64 sentinel
        case 12: put16(bytes + layout.central + 4, 0x0314); put32(bytes + layout.central + 38, 0xa0000000); break; // Unix symlink
        case 13: put32(bytes + layout.central + 38, FILE_ATTRIBUTE_REPARSE_POINT); break;
        case 14: put16(bytes + 26, 0); put16(bytes + layout.central + 28, 0); break; // empty name
        case 15: bytes[layout.data + 3] ^= 1; expected = UpdatePackageError::crc; break;
        case 16: put16(bytes + layout.data + 132, 0x14c); repair_crc(bytes, layout); expected = UpdatePackageError::pe; break; // i386
        case 17: put16(bytes + layout.data + 150, 0x2022); repair_crc(bytes, layout); expected = UpdatePackageError::pe; break; // a DLL
        case 18: put32(bytes + layout.data + 128 + 264 + 12, 0); put32(bytes + layout.data + 128 + 40, 1);
            repair_crc(bytes, layout); expected = UpdatePackageError::pe; break; // virtual section/header overlap
        case 19: put32(bytes + layout.data + 128 + 24 + 16, 0); repair_crc(bytes, layout); expected = UpdatePackageError::pe; break; // no entry
        case 20: put16(bytes + layout.data, 0); repair_crc(bytes, layout); expected = UpdatePackageError::pe; break; // not MZ
        case 21: break; // a second member
        }
        check(admit(count) && !extract() && extracted.error == expected && !package.file && !present(staged),
            "package: signed malformed metadata/name/member count/CRC/PE refuses before creating the stage");
    }
    // An executable longer than one read chunk: reads continue across chunks
    // without reseeking, the final chunk is short and a fault in it is caught.
    constexpr u32 kOverlay = 2 * kUpdatePackageReadBufferBytes + 5000;
    u8* const long_bytes = static_cast<u8*>(VirtualAlloc(nullptr, kOverlay + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!long_bytes) { check(false, "package: multi-chunk fixture memory"); return; }
    bytes = long_bytes; policy.maximum_artifact_bytes = u64{kOverlay} + 4096;
    const auto lz = fixture(bytes, kOverlay);
    check(admit(lz.bytes) && extract() && package.bytes == kImageBytes + kOverlay && holds(staged, bytes + lz.data, lz.size),
        "package: an executable spanning several read chunks extracts and reads back");
    bytes[lz.data + lz.size - 1] ^= 1;
    check(admit(lz.bytes) && !extract() && extracted.error == UpdatePackageError::crc && !package.file && !present(staged),
        "package: a fault in the last read chunk fails the CRC before the stage is created");
    bytes = storage; policy.maximum_artifact_bytes = 65536; VirtualFree(long_bytes, 0, MEM_RELEASE);
}
} // namespace maicord
