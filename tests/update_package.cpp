#include "maicord/app/update_package.hpp"
#include "update_signed_fixture_helpers.hpp"

namespace maicord {
namespace {
void put16(u8* p, u16 v) noexcept { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
void put32(u8* p, u32 v) noexcept { put16(p, static_cast<u16>(v)); put16(p + 2, static_cast<u16>(v >> 16)); }
u32 crc32(const u8* data, u32 count) noexcept {
    u32 crc = 0xffffffff;
    for (u32 i = 0; i < count; ++i) {
        crc ^= data[i];
        for (u32 bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return crc ^ 0xffffffff;
}
// Generated PE format fixtures, never executed/loaded. Real shipping startup
// admission is separately covered by root's built MaiCord integration.
constexpr u32 kImageBytes = 1024;
u32 pe(u8* out, bool dll) noexcept {
    SecureZeroMemory(out, kImageBytes);
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
    return kImageBytes;
}
// The launch target, a library and a data file; names of equal length so a
// mutation can rename one member in place.
constexpr u32 kMembers = 3, kNotesBytes = 64;
constexpr const char* kNames[kMembers]{"MaiCord.exe", "codec.dll", "notes.txt"};
constexpr u32 kNameBytes[kMembers]{11, 9, 9};
struct ZipLayout { u32 bytes, local[kMembers], data[kMembers], central[kMembers], sizes[kMembers], end; };
ZipLayout fixture(u8* out, u32 members = kMembers) noexcept {
    SecureZeroMemory(out, 4096);
    ZipLayout z{}; u32 offset = 0, crcs[kMembers]{};
    for (u32 i = 0; i < members; ++i) {
        z.local[i] = offset; u8* h = out + offset;
        put32(h, 0x04034b50); put16(h + 4, 20); put16(h + 26, static_cast<u16>(kNameBytes[i]));
        copy_bytes(h + 30, kNames[i], kNameBytes[i]); z.data[i] = offset + 30 + kNameBytes[i];
        if (i < 2) z.sizes[i] = pe(out + z.data[i], i == 1);
        else { z.sizes[i] = kNotesBytes; for (u32 b = 0; b < kNotesBytes; ++b) out[z.data[i] + b] = static_cast<u8>('a' + b % 26); }
        crcs[i] = crc32(out + z.data[i], z.sizes[i]);
        put32(h + 14, crcs[i]); put32(h + 18, z.sizes[i]); put32(h + 22, z.sizes[i]);
        offset = z.data[i] + z.sizes[i];
    }
    const u32 central_start = offset;
    for (u32 i = 0; i < members; ++i) {
        z.central[i] = offset; u8* h = out + offset;
        put32(h, 0x02014b50); put16(h + 4, 20); put16(h + 6, 20); put32(h + 16, crcs[i]);
        put32(h + 20, z.sizes[i]); put32(h + 24, z.sizes[i]); put16(h + 28, static_cast<u16>(kNameBytes[i]));
        put32(h + 42, z.local[i]); copy_bytes(h + 46, kNames[i], kNameBytes[i]); offset += 46 + kNameBytes[i];
    }
    z.end = offset; u8* h = out + offset; put32(h, 0x06054b50); put16(h + 8, static_cast<u16>(members)); put16(h + 10, static_cast<u16>(members));
    put32(h + 12, offset - central_start); put32(h + 16, central_start); z.bytes = offset + 22; return z;
}
void repair_crc(u8* bytes, const ZipLayout& z, u32 index) noexcept {
    const u32 crc = crc32(bytes + z.data[index], z.sizes[index]);
    put32(bytes + z.local[index] + 14, crc); put32(bytes + z.central[index] + 16, crc);
}
// Renames member index in both its local and central headers.
void rename(u8* bytes, const ZipLayout& z, u32 index, const char* name) noexcept {
    copy_bytes(bytes + z.local[index] + 30, name, kNameBytes[index]);
    copy_bytes(bytes + z.central[index] + 46, name, kNameBytes[index]);
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
struct Target {
    const wchar_t* directory = nullptr;
    wchar_t paths[kMembers][MAX_PATH]{};
    u32 created = 0;
    bool fail_second = false;
    bool fail_seal = false;
    bool protected_close = false;
};
bool validate(void*, DWORD*) noexcept { return true; }
bool create(void* opaque, Utf8View name, HANDLE* output, DWORD* native) noexcept {
    auto& target = *static_cast<Target*>(opaque);
    if (target.created == 1 && target.fail_second) { *native = ERROR_DISK_FULL; return false; }
    wchar_t wide[kUpdatePackageMaximumNameBytes + 1]{};
    for (u32 i = 0; i < name.bytes; ++i) wide[i] = static_cast<wchar_t>(name.data[i]);
    u32 slot = 0;
    while (slot + 1 < kMembers && (name.bytes != kNameBytes[slot] || !bytes_equal(name.data, kNames[slot], name.bytes))) ++slot;
    wchar_t* path = target.paths[slot];
    append(path, target.directory, wide);
    *output = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (*output == INVALID_HANDLE_VALUE) { *output = nullptr; *native = GetLastError(); return false; }
    ++target.created;
    if (target.protected_close) SetHandleInformation(*output, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE);
    return true;
}
bool seal(void* opaque, HANDLE* writable, HANDLE* retained, DWORD* native) noexcept {
    if (static_cast<Target*>(opaque)->fail_seal) { *native = ERROR_WRITE_FAULT; return false; }
    if (!DuplicateHandle(GetCurrentProcess(), *writable, GetCurrentProcess(), retained, FILE_GENERIC_READ | DELETE, FALSE, 0) ||
        !CloseHandle(*writable)) { *native = GetLastError(); return false; }
    *writable = nullptr; return true;
}
bool no_files(const UpdatePackage& package) noexcept {
    for (const auto file : package.files) if (file) return false;
    return true;
}
} // namespace

u32 signed_update_fixture::native_package(Slice<u8> output) noexcept {
    if (!output.data || output.count < 4096) return 0;
    return fixture(output.data).bytes;
}

void test_update_package(void (*check)(bool, const char*) noexcept) noexcept {
    const auto valid = [](const char* text) noexcept { u32 n = 0; while (text[n]) ++n; return update_package_member_name_valid({text, n}); };
    check(valid("MaiCord.exe") && valid("codec.dll") && valid("a") && valid("x-1_2.bin") && valid("README") &&
        !valid("") && !valid(".hidden") && !valid("-x") && !valid("dir/a") && !valid("dir\\a") && !valid("a:b") && !valid("a b") &&
        !valid("trailing.") && !valid("OFFER.JSON") && !valid("offer.sig") && !valid("package.zip") && !valid("nul") &&
        !valid("CON.txt") && !valid("com1.dll") && !valid("lpt9") && !valid("0123456789abcdef0123456789abcdefX"),
        "package: member names are bounded portable file names, never devices or provenance files");
    wchar_t temporary[MAX_PATH]{}, directory[MAX_PATH]{}, artifact[MAX_PATH]{}, acceptance_path[MAX_PATH]{}, lock_path[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, temporary);
    if (!n || n >= MAX_PATH || !GetTempFileNameW(temporary, L"MPP", 0, directory)) { check(false, "package: owned isolated fixture path"); return; }
    if (!DeleteFileW(directory) || !CreateDirectoryW(directory, nullptr)) { check(false, "package: owned fixture directory"); return; }
    append(artifact, directory, L"artifact.zip"); append(acceptance_path, directory, L"acceptance.bin");
    u32 lock_count = characters(acceptance_path); copy_bytes(lock_path, acceptance_path, lock_count * sizeof(wchar_t));
    copy_bytes(lock_path + lock_count, L".lock", 6 * sizeof(wchar_t));
    Target target{}; target.directory = directory;
    KdfSha256Workspace workspace{}; P256Key signer{}, key{}; UpdatePreparationContext preparation{}; UpdateStage stage{};
    UpdatePreparationResult prepared{}; UpdatePackage package{}; UpdatePackageResult extracted{};
    struct Cleanup {
        KdfSha256Workspace& workspace; P256Key& signer; P256Key& key; UpdatePreparationContext& preparation;
        UpdateStage& stage; UpdatePreparationResult& prepared; UpdatePackage& package; UpdatePackageResult& extracted;
        const wchar_t* directory; const wchar_t* artifact; const wchar_t* acceptance; const wchar_t* lock;
        ~Cleanup() noexcept {
            update_package_cleanup_release(&extracted); update_package_release(&package, true);
            update_stage_cleanup_release(&prepared.stage); update_stage_release(&stage, true); update_preparation_release(&preparation);
            p256_release(&key); p256_release(&signer); kdf_sha256_release(&workspace);
            DeleteFileW(artifact); DeleteFileW(acceptance); DeleteFileW(lock); RemoveDirectoryW(directory);
        }
    } cleanup{workspace, signer, key, preparation, stage, prepared, package, extracted, directory, artifact, acceptance_path, lock_path};
    u8 scalar[32]{}; scalar[31] = 1; u8 point[65]{}, signature[64]{}, digest[32]{};
    const bool crypto = kdf_sha256_initialize(&workspace).ok() && p256_import_private(&signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(signer, {point, 65}).ok() && p256_import_public(&key, P256Kind::ecdsa, {point, 65}).ok();
    check(crypto, "package: independent fixture signing/trusted key initializes"); if (!crypto) return;
    u8 bytes[4096]{}; char text[512]{}; u32 scratch[256]{}; u64 version = 6;
    UpdatePackageOffer offer{}; UpdateManifestPolicy policy{4, 0, 100, 65536, 1000, UpdateChannel::stable};
    const UpdatePackageTarget destination{&target, validate, create, seal};
    auto admit = [&](u32 count) noexcept {
        if (!update_package_cleanup_release(&extracted).ok() || !update_package_release(&package, true).ok() ||
            !update_stage_release(&stage, true).ok() || !update_stage_cleanup_release(&prepared.stage).ok() ||
            !update_preparation_release(&preparation).ok()) return false;
        const ConstSlice<u8> package_parts[]{{bytes, count}};
        if (!sha256(workspace, {package_parts, 1}, {digest, 32}).ok()) return false;
        offer.document = signed_update_fixture::manifest(text, count, digest, version++);
        offer.signature = {signature, 64};
        const ConstSlice<u8> signed_parts[]{
            {reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
            {reinterpret_cast<const u8*>(offer.document.data), offer.document.bytes}};
        if (!sha256(workspace, {signed_parts, 2}, {digest, 32}).ok() ||
            !p256_sign_digest(signer, {digest, 32}, {signature, 64}).ok() || !write(artifact, bytes, count)) return false;
        const UpdatePreparationRequest request{{acceptance_path, characters(acceptance_path)}, {artifact, characters(artifact)},
            {directory, characters(directory)}, offer.document, offer.signature, true};
        prepared = update_prepare_stage(&preparation, request, key, workspace, &policy, {scratch, 256}, &stage);
        target.created = 0;
        return prepared.ok();
    };
    auto extract = [&]() noexcept {
        extracted = update_package_extract(&stage, offer, key, workspace, policy, preparation.acceptance, {scratch, 256}, destination, &package);
        return extracted.ok();
    };
    const auto z = fixture(bytes);
    const auto& d = package.descriptor;
    check(admit(z.bytes) && extract() && target.created == kMembers && package.files[0] && package.files[1] && package.files[2] && !package.files[3] &&
        d.member_count == kMembers && d.members[0].bytes == kImageBytes && d.members[1].bytes == kImageBytes && d.members[2].bytes == kNotesBytes &&
        d.members[1].name_bytes == 9 && bytes_equal(d.members[1].name, "codec.dll", 9) && d.version == 6 && update_package_descriptor_valid(d),
        "package: current signed stored ZIP extracts the executable, a library and a data member offline");
    if (!package.files[0] || !package.files[2]) return;
    DWORD written = 0; const u8 changed = 1;
    check(!WriteFile(package.files[2], &changed, 1, &written, nullptr) && GetLastError() == ERROR_ACCESS_DENIED,
        "package: published member capability denies writes");
    UpdatePackageDescriptor reopened{};
    check(update_package_verify_installed(&stage, offer, key, workspace, 1000, {scratch, 256}, {package.files, kMembers}, &reopened).ok() &&
        update_package_descriptor_equal(reopened, d),
        "package: installed verification derives authority anew from signed retained package and verifies immutable members");
    const HANDLE swapped[]{package.files[0], package.files[2], package.files[1]};
    check(!update_package_verify_installed(&stage, offer, key, workspace, 1000, {scratch, 256}, {swapped, kMembers}, &reopened).ok() &&
        !update_package_verify_installed(&stage, offer, key, workspace, 1000, {scratch, 256}, {package.files, 2}, &reopened).ok(),
        "package: installed verification refuses members out of order or missing");
    const auto saved = package;
    check(update_package_extract(&stage, offer, key, workspace, policy, preparation.acceptance,
        {reinterpret_cast<u32*>(&package), 8}, destination, &package).error == UpdatePackageError::invalid_input &&
        bytes_equal(&saved, &package, sizeof(package)), "package: alias and already-owned output refuse mutation");
    check(update_package_release(&package, true).ok() && no_files(package), "package: rollback discards only owned member handles");
    const u64 high_water = preparation.acceptance.state.channels[0].highest_version;
    check(high_water == 6 && extract() && package.descriptor.version == high_water,
        "package: same admitted offer retries after candidate rollback without lowering acceptance");
    check(update_package_release(&package, true).ok(), "package: retry candidate release");
    // No handle metadata/hash supplied by a caller can authenticate different
    // bytes. This separately-owned damaged source has the same apparent size.
    wchar_t damaged_path[MAX_PATH]{}; append(damaged_path, directory, L"damaged.zip");
    bytes[z.data[0] + 600] ^= 1;
    HANDLE damaged = CreateFileW(damaged_path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (damaged != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        const bool written_ok = WriteFile(damaged, bytes, z.bytes, &wrote, nullptr) && wrote == z.bytes;
        UpdateStage forged = stage; forged.file = damaged;
        const auto refused = update_package_extract(&forged, offer, key, workspace, policy, preparation.acceptance,
            {scratch, 256}, destination, &package);
        check(written_ok && refused.error == UpdatePackageError::digest && no_files(package),
            "package: copied caller hashes and retained handle cannot authenticate modified outer package bytes");
        check(update_stage_release(&forged, true).ok(), "package: damaged fixture source disposed only by owned handle");
    } else check(false, "package: damaged source fixture CREATE_NEW");
    fixture(bytes);
    UpdateStage short_stage = stage; --short_stage.bytes;
    const auto short_result = update_package_extract(&short_stage, offer, key, workspace, policy, preparation.acceptance,
        {scratch, 256}, destination, &package);
    UpdateStage oversized_stage = stage; oversized_stage.bytes = kUpdatePackageMaximumBytes + 1;
    const auto oversized_result = update_package_extract(&oversized_stage, offer, key, workspace, policy, preparation.acceptance,
        {scratch, 256}, destination, &package);
    check(short_result.error == UpdatePackageError::stage && oversized_result.error == UpdatePackageError::stage,
        "package: truncated/oversized retained stage metadata refuses signed size mismatch");
    const u32 creations_before_signature = target.created;
    signature[0] ^= 1;
    check(!extract() && extracted.error == UpdatePackageError::offer && target.created == creations_before_signature,
        "package: changed signature refuses destination creation"); signature[0] ^= 1;
    UpdateManifestPolicy stale_policy = policy; stale_policy.now_unix_seconds = 200;
    const auto stale = update_package_extract(&stage, offer, key, workspace, stale_policy, preparation.acceptance,
        {scratch, 256}, destination, &package);
    check(stale.error == UpdatePackageError::offer && stale.manifest_error == UpdateManifestError::time,
        "package: expired current offer fails while installed verification has its separate recovery admission");
    const auto single = fixture(bytes, 1);
    check(admit(single.bytes) && extract() && package.descriptor.member_count == 1 && package.files[0] && !package.files[1],
        "package: the executable alone is a complete package");
    check(update_package_release(&package, true).ok(), "package: single-member candidate release");
    for (u32 mutation = 0; mutation < 32; ++mutation) {
        fixture(bytes); u32 count = z.bytes; UpdatePackageError expected = UpdatePackageError::zip;
        switch (mutation) {
        case 0: put16(bytes + 6, 1); break; // encryption
        case 1: put16(bytes + 6, 8); break; // descriptor
        case 2: put16(bytes + 8, 8); break; // deflate
        case 3: put16(bytes + 28, 1); break; // extra
        case 4: put16(bytes + z.central[0] + 32, 1); break; // comment
        case 5: bytes[count++] = 0; break; // appended byte
        case 6: --count; break; // truncated end
        case 7: put32(bytes + z.central[1] + 42, 0); break; // overlapping local
        case 8: bytes[z.local[2] + 30] = 'N'; break; // local/central name mismatch
        case 9: rename(bytes, z, 0, "maicord.exe"); break; // launch target is exact
        case 10: rename(bytes, z, 2, "CODEC.DLL"); break; // case-insensitive duplicate
        case 11: rename(bytes, z, 2, "offer.sig"); break; // provenance name
        case 12: rename(bytes, z, 2, "nul.a.txt"); break; // device name
        case 13: rename(bytes, z, 2, "notes/txt"); break;
        case 14: rename(bytes, z, 2, "notes\\txt"); break;
        case 15: rename(bytes, z, 2, "notes:txt"); break;
        case 16: rename(bytes, z, 2, ".notestxt"); break;
        case 17: rename(bytes, z, 2, "notes.tx."); break;
        case 18: bytes[z.local[2] + 30 + 4] = 0; bytes[z.central[2] + 46 + 4] = 0; break; // embedded NUL
        case 19: put16(bytes + z.end + 10, kMembers + 1); break;
        case 20: put16(bytes + z.end + 8, kUpdatePackageMaximumMembers + 1); put16(bytes + z.end + 10, kUpdatePackageMaximumMembers + 1); break;
        case 21: put32(bytes + 18, 0xffffffff); put32(bytes + 22, 0xffffffff); break;
        case 22: put16(bytes + z.central[0] + 4, 0x0314); put32(bytes + z.central[0] + 38, 0xa0000000); break; // Unix symlink
        case 23: put32(bytes + z.central[0] + 38, FILE_ATTRIBUTE_REPARSE_POINT); break;
        case 24: bytes[z.data[2] + 3] ^= 1; expected = UpdatePackageError::crc; break;
        case 25: put16(bytes + z.data[0] + 132, 0x14c); repair_crc(bytes, z, 0); expected = UpdatePackageError::pe; break;
        case 26: put16(bytes + z.data[1] + 150, 0x22); repair_crc(bytes, z, 1); expected = UpdatePackageError::pe; break; // .dll not a DLL
        case 27: put16(bytes + z.data[0] + 150, 0x2022); repair_crc(bytes, z, 0); expected = UpdatePackageError::pe; break; // .exe is a DLL
        case 28: rename(bytes, z, 2, "notes.EXE"); expected = UpdatePackageError::pe; break; // data named as an image
        case 29: put32(bytes + z.data[0] + 128 + 264 + 12, 0); put32(bytes + z.data[0] + 128 + 40, 1);
            repair_crc(bytes, z, 0); expected = UpdatePackageError::pe; break; // virtual section/header overlap
        case 30: put32(bytes + z.data[1] + 128 + 24 + 16, 0); repair_crc(bytes, z, 1); expected = UpdatePackageError::pe; break; // no entry section
        case 31: put16(bytes + z.local[0] + 26, 0); put16(bytes + z.central[0] + 28, 0); break; // empty name
        }
        check(admit(count) && !extract() && extracted.error == expected && no_files(package) && !target.created,
            "package: signed malformed metadata/name/overlap/CRC/PE fixture refuses every output before creation");
    }
    fixture(bytes); target.fail_second = true;
    check(admit(z.bytes) && !extract() && extracted.error == UpdatePackageError::create && target.created == 1 &&
        !extracted.cleanup_files[0] && !extracted.cleanup_files[kUpdatePackageMaximumMembers] &&
        GetFileAttributesW(target.paths[0]) == INVALID_FILE_ATTRIBUTES,
        "package: second member creation failure removes only first exclusively owned candidate"); target.fail_second = false;
    fixture(bytes); target.fail_seal = true; target.protected_close = true;
    check(admit(z.bytes) && !extract() && extracted.error == UpdatePackageError::seal && extracted.cleanup_error && extracted.cleanup_files[0],
        "package: failed close after seal refusal retains explicit disposal owner");
    if (extracted.cleanup_files[0]) {
        check(SetHandleInformation(extracted.cleanup_files[0], HANDLE_FLAG_PROTECT_FROM_CLOSE, 0) &&
            update_package_cleanup_release(&extracted).ok() && !extracted.cleanup_files[0],
            "package: clearing fixture close protection permits cleanup retry without losing owner");
    }
    target.fail_seal = false; target.protected_close = false;
    fixture(bytes); check(admit(z.bytes) && extract(), "package: valid package retries after failed cleanup ownership is released");
}
} // namespace maicord
