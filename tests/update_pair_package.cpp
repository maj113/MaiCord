#include "maicord/app/update_pair_package.hpp"
#include "maicord/app/account_module_api.hpp"
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
// Generated PE format fixtures, never executed/loaded. Real shipping ABI/startup
// admission is separately covered by root's built MaiCord pair integration.
u32 pe(u8* out, bool dll) noexcept {
    const u32 bytes = dll ? 1536 : 1024;
    SecureZeroMemory(out, bytes);
    put16(out, 0x5a4d); put32(out + 60, 128);
    u8* nt = out + 128; put32(nt, 0x00004550); put16(nt + 4, 0x8664);
    put16(nt + 6, dll ? 2 : 1); put16(nt + 20, 240); put16(nt + 22, dll ? 0x2022 : 0x22);
    u8* opt = nt + 24; put16(opt, 0x20b); opt[2] = 14;
    put32(opt + 4, 512); put32(opt + 8, dll ? 512 : 0); put32(opt + 16, 4096); put32(opt + 20, 4096);
    put32(opt + 24, 0x400000); put32(opt + 32, 4096); put32(opt + 36, 512);
    put16(opt + 40, 6); put16(opt + 48, 6); put32(opt + 56, dll ? 12288 : 8192);
    put32(opt + 60, 512); put16(opt + 68, 3); put16(opt + 70, 0x100);
    put32(opt + 72, 1048576); put32(opt + 80, 4096); put32(opt + 88, 1048576); put32(opt + 96, 4096);
    put32(opt + 108, 16);
    u8* sec = nt + 264; copy_bytes(sec, ".text", 5); put32(sec + 8, 6); put32(sec + 12, 4096);
    put32(sec + 16, 512); put32(sec + 20, 512); put32(sec + 36, 0x60000020);
    out[512] = 0xb8; out[513] = 1; out[517] = 0xc3;
    if (dll) {
        sec += 40; copy_bytes(sec, ".rdata", 6); put32(sec + 8, 80); put32(sec + 12, 8192);
        put32(sec + 16, 512); put32(sec + 20, 1024); put32(sec + 36, 0x40000040);
        put32(opt + 112, 8192); put32(opt + 116, 80);
        u8* ex = out + 1024; put32(ex + 12, 8192 + 44); put32(ex + 16, kAccountModuleApiOrdinal);
        put32(ex + 20, 1); put32(ex + 28, 8192 + 40); put32(ex + 40, 4096);
        copy_bytes(ex + 44, "MaiCordAccount.dll", 19);
    }
    return bytes;
}
struct ZipLayout { u32 bytes, local[2], data[2], central[2], end; };
ZipLayout package(u8* out) noexcept {
    SecureZeroMemory(out, 4096);
    constexpr char names[2][19]{"MaiCord.exe", "MaiCordAccount.dll"};
    constexpr u32 lengths[2]{11, 18};
    ZipLayout z{}; u32 offset = 0; u32 sizes[2]{}, crcs[2]{};
    for (u32 i = 0; i < 2; ++i) {
        z.local[i] = offset; u8* h = out + offset;
        put32(h, 0x04034b50); put16(h + 4, 20); put16(h + 26, static_cast<u16>(lengths[i]));
        copy_bytes(h + 30, names[i], lengths[i]); z.data[i] = offset + 30 + lengths[i];
        sizes[i] = pe(out + z.data[i], i == 1); crcs[i] = crc32(out + z.data[i], sizes[i]);
        put32(h + 14, crcs[i]); put32(h + 18, sizes[i]); put32(h + 22, sizes[i]);
        offset = z.data[i] + sizes[i];
    }
    const u32 central_start = offset;
    for (u32 i = 0; i < 2; ++i) {
        z.central[i] = offset; u8* h = out + offset;
        put32(h, 0x02014b50); put16(h + 4, 20); put16(h + 6, 20); put32(h + 16, crcs[i]);
        put32(h + 20, sizes[i]); put32(h + 24, sizes[i]); put16(h + 28, static_cast<u16>(lengths[i]));
        put32(h + 42, z.local[i]); copy_bytes(h + 46, names[i], lengths[i]); offset += 46 + lengths[i];
    }
    z.end = offset; u8* h = out + offset; put32(h, 0x06054b50); put16(h + 8, 2); put16(h + 10, 2);
    put32(h + 12, offset - central_start); put32(h + 16, central_start); z.bytes = offset + 22; return z;
}
void repair_crc(u8* bytes, const ZipLayout& z, u32 index) noexcept {
    const u32 size = index ? 1536 : 1024;
    const u32 crc = crc32(bytes + z.data[index], size);
    put32(bytes + z.local[index] + 14, crc); put32(bytes + z.central[index] + 16, crc);
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
    wchar_t paths[2][MAX_PATH]{};
    u32 created = 0;
    bool fail_second = false;
    bool fail_seal = false;
    bool protected_close = false;
};
bool validate(void*, DWORD*) noexcept { return true; }
bool create(void* opaque, u32 index, HANDLE* output, DWORD* native) noexcept {
    auto& target = *static_cast<Target*>(opaque);
    if (index == 1 && target.fail_second) { *native = ERROR_DISK_FULL; return false; }
    *output = CreateFileW(target.paths[index], GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (*output == INVALID_HANDLE_VALUE) { *output = nullptr; *native = GetLastError(); return false; }
    ++target.created;
    if (target.protected_close) SetHandleInformation(*output, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE);
    return true;
}
bool seal(void* opaque, u32, HANDLE* writable, HANDLE* retained, DWORD* native) noexcept {
    if (static_cast<Target*>(opaque)->fail_seal) { *native = ERROR_WRITE_FAULT; return false; }
    if (!DuplicateHandle(GetCurrentProcess(), *writable, GetCurrentProcess(), retained, FILE_GENERIC_READ | DELETE, FALSE, 0) ||
        !CloseHandle(*writable)) { *native = GetLastError(); return false; }
    *writable = nullptr; return true;
}
} // namespace

void test_update_pair_package(void (*check)(bool, const char*) noexcept) noexcept {
    wchar_t temporary[MAX_PATH]{}, directory[MAX_PATH]{}, artifact[MAX_PATH]{}, acceptance_path[MAX_PATH]{}, lock_path[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, temporary);
    if (!n || n >= MAX_PATH || !GetTempFileNameW(temporary, L"MPP", 0, directory)) { check(false, "pair: owned isolated fixture path"); return; }
    if (!DeleteFileW(directory) || !CreateDirectoryW(directory, nullptr)) { check(false, "pair: owned fixture directory"); return; }
    append(artifact, directory, L"artifact.zip"); append(acceptance_path, directory, L"acceptance.bin");
    u32 lock_count = characters(acceptance_path); copy_bytes(lock_path, acceptance_path, lock_count * sizeof(wchar_t));
    copy_bytes(lock_path + lock_count, L".lock", 6 * sizeof(wchar_t));
    Target target{}; append(target.paths[0], directory, L"MaiCord.exe"); append(target.paths[1], directory, L"MaiCordAccount.dll");
    KdfSha256Workspace workspace{}; P256Key signer{}, key{}; UpdatePreparationContext preparation{}; UpdateStage stage{};
    UpdatePreparationResult prepared{}; UpdatePair pair{}; UpdatePairResult extracted{};
    struct Cleanup {
        KdfSha256Workspace& workspace; P256Key& signer; P256Key& key; UpdatePreparationContext& preparation;
        UpdateStage& stage; UpdatePreparationResult& prepared; UpdatePair& pair; UpdatePairResult& extracted;
        const wchar_t* directory; const wchar_t* artifact; const wchar_t* acceptance; const wchar_t* lock;
        ~Cleanup() noexcept {
            update_pair_cleanup_release(&extracted); update_pair_release(&pair, true);
            update_stage_cleanup_release(&prepared.stage); update_stage_release(&stage, true); update_preparation_release(&preparation);
            p256_release(&key); p256_release(&signer); kdf_sha256_release(&workspace);
            DeleteFileW(artifact); DeleteFileW(acceptance); DeleteFileW(lock); RemoveDirectoryW(directory);
        }
    } cleanup{workspace, signer, key, preparation, stage, prepared, pair, extracted, directory, artifact, acceptance_path, lock_path};
    u8 scalar[32]{}; scalar[31] = 1; u8 point[65]{}, signature[64]{}, digest[32]{};
    const bool crypto = kdf_sha256_initialize(&workspace).ok() && p256_import_private(&signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(signer, {point, 65}).ok() && p256_import_public(&key, P256Kind::ecdsa, {point, 65}).ok();
    check(crypto, "pair: independent fixture signing/trusted key initializes"); if (!crypto) return;
    u8 bytes[4096]{}; char text[512]{}; u32 scratch[256]{}; u64 version = 6;
    UpdatePairOffer offer{}; UpdateManifestPolicy policy{4, 0, 100, 65536, 1000, UpdateChannel::stable};
    const UpdatePairTarget destination{&target, validate, create, seal};
    auto admit = [&](u32 count) noexcept {
        if (!update_pair_cleanup_release(&extracted).ok() || !update_pair_release(&pair, true).ok() ||
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
        extracted = update_pair_package_extract(&stage, offer, key, workspace, policy, preparation.acceptance, {scratch, 256}, destination, &pair);
        return extracted.ok();
    };
    const auto z = package(bytes);
    check(admit(z.bytes) && extract() && target.created == 2 && pair.files[0] && pair.files[1] &&
        pair.descriptor.members[0].bytes == 1024 && pair.descriptor.members[1].bytes == 1536 &&
        pair.descriptor.account_ordinal == kAccountModuleApiOrdinal && pair.descriptor.version == 6,
        "pair: current signed stored ZIP extracts exact complete AMD64 pair and ordinal offline");
    if (!pair.files[0] || !pair.files[1]) return;
    DWORD written = 0; const u8 changed = 1;
    check(!WriteFile(pair.files[0], &changed, 1, &written, nullptr) && GetLastError() == ERROR_ACCESS_DENIED,
        "pair: published member capability denies writes");
    UpdatePairDescriptor reopened{};
    check(update_pair_package_verify_installed(&stage, offer, key, workspace, 1000, {scratch, 256}, pair.files[0], pair.files[1], &reopened).ok() &&
        bytes_equal(reopened.members[0].sha256, pair.descriptor.members[0].sha256, 32),
        "pair: installed verification derives authority anew from signed retained package and verifies immutable members");
    const auto saved = pair;
    check(update_pair_package_extract(&stage, offer, key, workspace, policy, preparation.acceptance,
        {reinterpret_cast<u32*>(&pair), 8}, destination, &pair).error == UpdatePairError::invalid_input &&
        bytes_equal(&saved, &pair, sizeof(pair)), "pair: alias and already-owned output refuse mutation");
    check(update_pair_release(&pair, true).ok(), "pair: rollback discards only owned pair handles");
    const u64 high_water = preparation.acceptance.state.channels[0].highest_version;
    check(high_water == 6 && extract() && pair.descriptor.version == high_water,
        "pair: same admitted offer retries after candidate rollback without lowering acceptance");
    check(update_pair_release(&pair, true).ok(), "pair: retry candidate release");
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
        const auto refused = update_pair_package_extract(&forged, offer, key, workspace, policy, preparation.acceptance,
            {scratch, 256}, destination, &pair);
        check(written_ok && refused.error == UpdatePairError::digest && !pair.files[0] && !pair.files[1],
            "pair: copied caller hashes and retained handle cannot authenticate modified outer package bytes");
        check(update_stage_release(&forged, true).ok(), "pair: damaged fixture source disposed only by owned handle");
    } else check(false, "pair: damaged source fixture CREATE_NEW");
    package(bytes);
    UpdateStage short_stage = stage; --short_stage.bytes;
    const auto short_result = update_pair_package_extract(&short_stage, offer, key, workspace, policy, preparation.acceptance,
        {scratch, 256}, destination, &pair);
    UpdateStage oversized_stage = stage; oversized_stage.bytes = kUpdatePairMaximumPackageBytes + 1;
    const auto oversized_result = update_pair_package_extract(&oversized_stage, offer, key, workspace, policy, preparation.acceptance,
        {scratch, 256}, destination, &pair);
    check(short_result.error == UpdatePairError::stage && oversized_result.error == UpdatePairError::stage,
        "pair: truncated/oversized retained stage metadata refuses signed size mismatch");
    const u32 creations_before_signature = target.created;
    signature[0] ^= 1;
    check(!extract() && extracted.error == UpdatePairError::offer && target.created == creations_before_signature,
        "pair: changed signature refuses destination creation"); signature[0] ^= 1;
    UpdateManifestPolicy stale_policy = policy; stale_policy.now_unix_seconds = 200;
    const auto stale = update_pair_package_extract(&stage, offer, key, workspace, stale_policy, preparation.acceptance,
        {scratch, 256}, destination, &pair);
    check(stale.error == UpdatePairError::offer && stale.manifest_error == UpdateManifestError::time,
        "pair: expired current offer fails while installed verification has its separate recovery admission");
    // The installer must accept the account API of the release it installs:
    // v0.1.0 pinned its own ordinal and refused every later package.
    package(bytes); put32(bytes + z.data[1] + 1024 + 16, kAccountModuleApiOrdinal + 7); repair_crc(bytes, z, 1);
    check(admit(z.bytes) && extract() && pair.descriptor.account_ordinal == kAccountModuleApiOrdinal + 7,
        "pair: a package exporting a newer account API ordinal installs and records that ordinal");
    check(update_pair_release(&pair, true).ok(), "pair: newer-API candidate release");
    for (u32 mutation = 0; mutation < 23; ++mutation) {
        package(bytes); u32 count = z.bytes; UpdatePairError expected = UpdatePairError::zip;
        switch (mutation) {
        case 0: put16(bytes + 6, 1); break; // encryption
        case 1: put16(bytes + 6, 8); break; // descriptor
        case 2: put16(bytes + 8, 8); break; // deflate
        case 3: put16(bytes + 28, 1); break; // extra
        case 4: put16(bytes + z.central[0] + 32, 1); break; // comment
        case 5: bytes[count++] = 0; break; // appended byte
        case 6: --count; break; // truncated end
        case 7: put32(bytes + z.central[1] + 42, 0); break; // overlapping local
        case 8: bytes[30] = 'm'; break; // case alias
        case 9: bytes[30] = '/'; break;
        case 10: bytes[30] = '\\'; break;
        case 11: bytes[30] = ':'; break;
        case 12: bytes[30] = 0; break;
        case 13: put16(bytes + z.end + 10, 3); break;
        case 14: put32(bytes + 18, 0xffffffff); put32(bytes + 22, 0xffffffff); break;
        case 15: put16(bytes + z.central[0] + 4, 0x0314); put32(bytes + z.central[0] + 38, 0xa0000000); break; // Unix symlink
        case 16: put32(bytes + z.central[0] + 38, FILE_ATTRIBUTE_REPARSE_POINT); break;
        case 17: bytes[z.data[0] + 600] ^= 1; expected = UpdatePairError::crc; break;
        case 18: put16(bytes + z.data[0] + 132, 0x14c); repair_crc(bytes, z, 0); expected = UpdatePairError::pe; break;
        case 19: put16(bytes + z.data[1] + 150, 0x22); repair_crc(bytes, z, 1); expected = UpdatePairError::pe; break;
        case 20: put32(bytes + z.data[1] + 1024 + 20, 0); repair_crc(bytes, z, 1); expected = UpdatePairError::pe; break; // no export
        case 21: put32(bytes + z.data[1] + 1024 + 40, 8192); repair_crc(bytes, z, 1); expected = UpdatePairError::pe; break; // forwarded export
        case 22: put32(bytes + z.data[0] + 128 + 264 + 12, 0); put32(bytes + z.data[0] + 128 + 40, 1);
            repair_crc(bytes, z, 0); expected = UpdatePairError::pe; break; // virtual section/header overlap
        }
        check(admit(count) && !extract() && extracted.error == expected && !pair.files[0] && !pair.files[1] && !target.created,
            "pair: signed malformed metadata/overlap/alias/CRC/PE fixture refuses every output before creation");
    }
    package(bytes); target.fail_second = true;
    check(admit(z.bytes) && !extract() && extracted.error == UpdatePairError::create && target.created == 1 &&
        !extracted.cleanup_files[0] && !extracted.cleanup_files[2] && GetFileAttributesW(target.paths[0]) == INVALID_FILE_ATTRIBUTES,
        "pair: second member creation failure removes only first exclusively owned candidate"); target.fail_second = false;
    package(bytes); target.fail_seal = true; target.protected_close = true;
    check(admit(z.bytes) && !extract() && extracted.error == UpdatePairError::seal && extracted.cleanup_error && extracted.cleanup_files[0],
        "pair: failed close after seal refusal retains explicit disposal owner");
    if (extracted.cleanup_files[0]) {
        check(SetHandleInformation(extracted.cleanup_files[0], HANDLE_FLAG_PROTECT_FROM_CLOSE, 0) &&
            update_pair_cleanup_release(&extracted).ok() && !extracted.cleanup_files[0],
            "pair: clearing fixture close protection permits cleanup retry without losing owner");
    }
    target.fail_seal = false; target.protected_close = false;
    package(bytes); check(admit(z.bytes) && extract(), "pair: valid package retries after failed cleanup ownership is released");
}
} // namespace maicord
