// Journals and generations written by releases up to 0.4.x: record v2 and
// protocol-1 MaiCord.exe/MaiCordAccount.dll packages. Every v2 phase decodes to
// the same v3 state, the generations stay verifiable under the publisher key,
// cold recovery follows the captured v2 file identities and the next commit
// writes v3. Synthetic packages and keys only.
#include "maicord/app/update_generation_authentication.hpp"
#include "update_signed_fixture_helpers.hpp"

namespace maicord {
using namespace signed_update_fixture;
namespace {
UpdatePackageDescriptor fixture_pair(u64 version, u8 marker) noexcept {
    u8 digests[4][32]{};
    for (u32 i = 0; i < 4; ++i) for (u32 b = 0; b < 32; ++b) digests[i][b] = static_cast<u8>(marker + i);
    return legacy_pair(version, 5000, digests[0], digests[1], 1200, digests[2], 800, digests[3]);
}
// Equal v3 encodings: every field matches and the state is writable as v3.
bool same(const UpdateGenerationState& a, const UpdateGenerationState& b) noexcept {
    u8 x[kUpdateGenerationRecordBytes]{}, y[kUpdateGenerationRecordBytes]{};
    return update_generation_encode(a, {x, sizeof(x)}).ok() && update_generation_encode(b, {y, sizeof(y)}).ok() && bytes_equal(x, y, sizeof(x));
}
bool legacy_references(const UpdateGenerationState& s) noexcept {
    const UpdateGenerationReference* references[]{&s.active, &s.previous, &s.candidate};
    for (auto* r : references) if (r->present && !update_package_descriptor_legacy(r->package)) return false;
    return true;
}
bool migrates(const UpdateGenerationState& expected, u16 ordinal = 135) noexcept {
    u8 record[kUpdateGenerationLegacyRecordBytes]{}; legacy_record(expected, ordinal, record);
    UpdateGenerationState decoded{};
    return update_generation_decode({record, sizeof(record)}, &decoded).ok() && same(decoded, expected) && legacy_references(decoded);
}
void rechecksum(u8* record) noexcept { byte_store_le64(record + 760, hash64_bytes(kHash64Seed, record, 760)); }
UpdateGenerationDisposalIntent fixture_intent(UpdateGenerationDisposalTarget target) noexcept {
    UpdateGenerationDisposalIntent intent{}; intent.target = target; intent.directory = {9, 100}; intent.file_count = 5;
    for (u32 i = 0; i < 5; ++i) intent.files[i] = {9, 101 + i};
    return intent;
}

// One protocol-1 generation as a 0.4.x client left it on disk.
struct LegacyGeneration {
    char document[512];
    u32 document_bytes;
    u8 signature[64];
    UpdatePackageDescriptor descriptor;
};
constexpr u32 kPairZipCapacity = 2 * (30 + 46 + 2 * 18 + kNativeImageBytes) + 22;
u32 pair_zip(u8* out, const u8* const* images, const char* const* names, u32 count) noexcept {
    clear_bytes(out, kPairZipCapacity);
    u32 offset = 0, local[2]{}, crcs[2]{}, name_bytes[2]{};
    for (u32 i = 0; i < count; ++i) {
        while (names[i][name_bytes[i]]) ++name_bytes[i];
        crcs[i] = crc(~u32{0}, images[i], kNativeImageBytes) ^ ~u32{0};
        local[i] = offset; u8* h = out + offset;
        put32(h, 0x04034b50); put16(h + 4, 20); put32(h + 14, crcs[i]); put32(h + 18, kNativeImageBytes); put32(h + 22, kNativeImageBytes);
        put16(h + 26, static_cast<u16>(name_bytes[i])); copy_bytes(h + 30, names[i], name_bytes[i]);
        copy_bytes(h + 30 + name_bytes[i], images[i], kNativeImageBytes); offset += 30 + name_bytes[i] + kNativeImageBytes;
    }
    const u32 central = offset;
    for (u32 i = 0; i < count; ++i) {
        u8* h = out + offset; put32(h, 0x02014b50); put16(h + 4, 20); put16(h + 6, 20); put32(h + 16, crcs[i]);
        put32(h + 20, kNativeImageBytes); put32(h + 24, kNativeImageBytes); put16(h + 28, static_cast<u16>(name_bytes[i]));
        put32(h + 42, local[i]); copy_bytes(h + 46, names[i], name_bytes[i]); offset += 46 + name_bytes[i];
    }
    u8* end = out + offset; put32(end, 0x06054b50); put16(end + 8, static_cast<u16>(count)); put16(end + 10, static_cast<u16>(count));
    put32(end + 12, offset - central); put32(end + 16, central); return offset + 22;
}
void generation_directory(const wchar_t* root, const u8* digest, wchar_t* out) noexcept {
    constexpr wchar_t hex[] = L"0123456789abcdef"; wchar_t name[67]{}; name[0] = L'g'; name[1] = L'-';
    for (u32 i = 0; i < 32; ++i) { name[2 + i * 2] = hex[digest[i] >> 4]; name[3 + i * 2] = hex[digest[i] & 15]; }
    child(root, name, out);
}
bool write_new(const wchar_t* path, const void* data, u32 bytes, UpdateGenerationDirectoryIdentity* id) noexcept {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = write(h, data, bytes) && FlushFileBuffers(h) && GetFileInformationByHandle(h, &info);
    *id = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
    return CloseHandle(h) && ok;
}
// The five fixed v2 files; identities by v3 slot, the directory's in *directory.
bool install_generation(const wchar_t* root, const LegacyGeneration& g, const u8* zip, u32 zip_bytes, const u8* exe, const u8* dll,
    UpdateGenerationDirectoryIdentity* directory, UpdateGenerationDirectoryIdentity* files) noexcept {
    wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{}; generation_directory(root, g.descriptor.document_sha256, dir);
    if (!CreateDirectoryW(dir, nullptr)) return false;
    HANDLE h = CreateFileW(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{}; const bool captured = GetFileInformationByHandle(h, &info) != FALSE;
    if (!CloseHandle(h) || !captured) return false;
    *directory = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
    const wchar_t* names[]{L"offer.json", L"offer.sig", L"package.zip", L"MaiCord.exe", L"MaiCordAccount.dll"};
    const void* data[]{g.document, g.signature, zip, exe, dll};
    const u32 sizes[]{g.document_bytes, 64, zip_bytes, kNativeImageBytes, kNativeImageBytes};
    for (u32 i = 0; i < 5; ++i) { child(dir, names[i], path); if (!write_new(path, data[i], sizes[i], &files[i])) return false; }
    return true;
}
// Teardown of this fixture's exact files; no enumeration.
void remove_generation(const wchar_t* root, const u8* digest) noexcept {
    wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{}; generation_directory(root, digest, dir);
    const wchar_t* names[]{L"offer.json", L"offer.sig", L"package.zip", L"MaiCord.exe", L"MaiCordAccount.dll"};
    for (auto name : names) { child(dir, name, path); DeleteFileW(path); }
    RemoveDirectoryW(dir);
}
bool exists(const wchar_t* path) noexcept { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
u64 file_bytes(const wchar_t* path) noexcept {
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size{}; const bool ok = GetFileSizeEx(h, &size) != FALSE;
    CloseHandle(h); return ok ? static_cast<u64>(size.QuadPart) : 0;
}
bool write_record(const wchar_t* path, const u8* record) noexcept {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = write(h, record, kUpdateGenerationLegacyRecordBytes) && FlushFileBuffers(h);
    return CloseHandle(h) && ok;
}
} // namespace

void test_update_generation_legacy(void (*check)(bool, const char*) noexcept) noexcept {
    // ------------------------------------------------------------ record codec
    UpdateGenerationState stable{}; stable.installation.bytes[0] = 0x5a; stable.sequence = 7;
    stable.active = {fixture_pair(4000999, 0x10), true};
    const auto next = fixture_pair(4001026, 0x40);
    UpdateGenerationIdentity transaction{}; transaction.bytes[3] = 0x77;
    UpdateGenerationState prepared{}, pending{}, acknowledged{}, updated{}, rollback{}, discarding{}, pruning{};
    const bool planned = update_generation_prepare(stable, transaction, next, &prepared).ok() && update_generation_activate(prepared, &pending).ok() &&
        update_generation_acknowledge(pending, transaction, {next.document_sha256, 32}, &acknowledged).ok() &&
        update_generation_finish(acknowledged, false, &updated).ok() && update_generation_rollback(pending, &rollback).ok() &&
        update_generation_disposal_begin(rollback, fixture_intent(UpdateGenerationDisposalTarget::candidate), &discarding).ok() &&
        update_generation_disposal_begin(updated, fixture_intent(UpdateGenerationDisposalTarget::previous), &pruning).ok();
    auto executable_only = next; executable_only.member_count = 1; executable_only.members[1] = {};
    check(planned && update_package_descriptor_valid(next) && update_package_descriptor_legacy(next) && update_package_descriptor_valid(executable_only) &&
        !update_package_descriptor_legacy(executable_only), "generation legacy: a v2 pair is a valid two-member v3 package that is recognisably legacy");
    check(migrates(stable) && migrates(updated) && migrates(pruning), "generation legacy: v2 stable records, with a previous pair and its disposal intent, decode to the same v3 state");
    check(migrates(prepared) && migrates(pending) && migrates(acknowledged), "generation legacy: v2 prepared, pending and acknowledged records decode to the same v3 transaction");
    check(migrates(rollback) && migrates(discarding), "generation legacy: v2 rollback records and their candidate disposal intent decode to the same v3 state");
    check(migrates(stable, 1) && migrates(updated, 0xffff), "generation legacy: the account DLL's export ordinal is dropped, whatever its value");
    u8 record[kUpdateGenerationLegacyRecordBytes + 1]{}; UpdateGenerationState decoded{};
    legacy_record(discarding, 135, record);
    const bool slots = update_generation_decode({record, kUpdateGenerationLegacyRecordBytes}, &decoded).ok() && decoded.disposal.file_count == 5 &&
        decoded.disposal.files[kUpdateGenerationFirstMemberSlot].file == byte_load_le64(record + 592 + 32) &&
        decoded.disposal.files[kUpdateGenerationFirstMemberSlot + 1].file == byte_load_le64(record + 592 + 48) &&
        decoded.disposal.files[kUpdateGenerationOfferSlot].file == byte_load_le64(record + 592 + 64) &&
        decoded.disposal.files[kUpdateGenerationPackageSlot].file == byte_load_le64(record + 592 + 96);
    check(slots && utf8_equal(update_generation_file_name(decoded.candidate.package, kUpdateGenerationFirstMemberSlot), kUpdatePackageExecutable) &&
        utf8_equal(update_generation_file_name(decoded.candidate.package, kUpdateGenerationFirstMemberSlot + 1), kUpdatePackageLegacyLibrary),
        "generation legacy: the v2 exe, dll, offer, signature, package identities land in the slots named for those files");

    const auto sentinel_state = stable; auto sentinel = sentinel_state;
    const auto refused = [&](UpdateGenerationError expected, u32 bytes = kUpdateGenerationLegacyRecordBytes) noexcept {
        sentinel = sentinel_state;
        return update_generation_decode({record, bytes}, &sentinel).error == expected && same(sentinel, sentinel_state);
    };
    const auto reset = [&](const UpdateGenerationState& s, u16 ordinal = 135) noexcept { legacy_record(s, ordinal, record); };
    reset(pending); record[64 + 90] ^= 1;
    check(refused(UpdateGenerationError::checksum), "generation legacy: a changed v2 digest without its checksum is refused");
    reset(pending); record[8] = 3; rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: a 768-byte record of another version is refused");
    reset(pending); record[7] = '3'; rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: a 768-byte record with another magic is refused");
    reset(pending); record[57] = 1; rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: reserved v2 header bytes stay zero");
    reset(pending); record[64 + 170] = 1; rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: reserved v2 reference bytes stay zero");
    reset(stable, 0);
    check(refused(UpdateGenerationError::format), "generation legacy: a v2 pair without its account ordinal is refused as v2 refused it");
    reset(stable); record[64 + 176 + 5] = 1; rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: an absent v2 reference is entirely zero");
    reset(stable); record[64 + 163] = 2; rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: an unknown v2 presence flag is refused");
    reset(discarding); record[593] = 1; rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: reserved v2 disposal bytes stay zero");
    reset(discarding); record[592] = 3; rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: an unknown v2 disposal target is refused");
    reset(stable); record[592] = static_cast<u8>(UpdateGenerationDisposalTarget::previous); rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: v2 disposal intent without its target and identities is refused");
    reset(pending); record[56] = static_cast<u8>(UpdateGenerationPhase::stable); rechecksum(record);
    check(refused(UpdateGenerationError::format), "generation legacy: a v2 phase that contradicts its references is refused");
    reset(stable);
    check(refused(UpdateGenerationError::size, kUpdateGenerationLegacyRecordBytes - 1) && refused(UpdateGenerationError::size, kUpdateGenerationLegacyRecordBytes + 1),
        "generation legacy: torn and appended v2 records are refused");

    // ------------------------------------------------------- signed generations
    KdfSha256Workspace workspace{}; P256Key signer{}, key{}; u8 scalar[32]{}, point[65]{}, inventory[32]{};
    scalar[31] = 1;
    const bool crypto = kdf_sha256_initialize(&workspace).ok() && p256_import_private(&signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(signer, {point, 65}).ok() && p256_import_public(&key, P256Kind::ecdsa, {point, 65}).ok() &&
        update_generation_inventory_fingerprint(key, workspace, {inventory, 32}).ok();
    struct CryptoCleanup { KdfSha256Workspace& w; P256Key& s; P256Key& k;
        ~CryptoCleanup() noexcept { p256_release(&k); p256_release(&s); kdf_sha256_release(&w); }
    } crypto_cleanup{workspace, signer, key};
    check(crypto, "generation legacy: fixture signer and enrolled public inventory initialize"); if (!crypto) return;
    u8 exe[kNativeImageBytes]{}, dll[kNativeImageBytes]{}, zip[kPairZipCapacity]{}, single[kPairZipCapacity]{};
    native_image(exe, false); native_image(dll, true);
    const u8* images[]{exe, dll}; const char* names[]{"MaiCord.exe", "MaiCordAccount.dll"};
    const u32 zip_bytes = pair_zip(zip, images, names, 2), single_bytes = pair_zip(single, images, names, 1);
    u8 zip_digest[32]{}, single_digest[32]{}, exe_digest[32]{}, dll_digest[32]{};
    const ConstSlice<u8> zip_part[]{{zip, zip_bytes}}, single_part[]{{single, single_bytes}}, exe_part[]{{exe, kNativeImageBytes}}, dll_part[]{{dll, kNativeImageBytes}};
    LegacyGeneration old_generation{}, new_generation{};
    const auto offer = [&](LegacyGeneration& g, u64 version) noexcept {
        const auto text = manifest(g.document, zip_bytes, zip_digest, version, kUpdateLegacyProtocol); g.document_bytes = text.bytes;
        u8 document_digest[32]{};
        const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
            {reinterpret_cast<const u8*>(text.data), text.bytes}};
        g.descriptor = legacy_pair(version, zip_bytes, document_digest, zip_digest, kNativeImageBytes, exe_digest, kNativeImageBytes, dll_digest);
        return sha256(workspace, {parts, 2}, {g.descriptor.document_sha256, 32}).ok() && sign(text, signer, workspace, g.signature);
    };
    const bool built = sha256(workspace, {zip_part, 1}, {zip_digest, 32}).ok() && sha256(workspace, {single_part, 1}, {single_digest, 32}).ok() &&
        sha256(workspace, {exe_part, 1}, {exe_digest, 32}).ok() && sha256(workspace, {dll_part, 1}, {dll_digest, 32}).ok() &&
        offer(old_generation, 4000999) && offer(new_generation, 4001026);
    check(built, "generation legacy: protocol-1 offers sign a stored MaiCord.exe and MaiCordAccount.dll pair"); if (!built) return;

    UpdateManifest parsed{}; u32 scratch[256]{};
    const Utf8View legacy_text{old_generation.document, old_generation.document_bytes};
    const UpdateManifestPolicy policy{1, 0, 100, 64 * 1024 * 1024, 1000, UpdateChannel::stable};
    check(update_manifest_verify_generation(legacy_text, {old_generation.signature, 64}, key, workspace, kUpdatePackageMaximumBytes, 1000, {scratch, 256}, &parsed).ok() &&
        parsed.protocol == kUpdateLegacyProtocol && parsed.version == 4000999 &&
        update_manifest_verify_installed(legacy_text, {old_generation.signature, 64}, key, workspace, kUpdatePackageMaximumBytes, 1000, {scratch, 256}, &parsed).error == UpdateManifestError::field &&
        update_manifest_verify(legacy_text, {old_generation.signature, 64}, key, workspace, policy, {scratch, 256}, &parsed).error == UpdateManifestError::field,
        "generation legacy: a protocol-1 offer verifies only as an installed generation, never as an update offer");

    wchar_t temp[MAX_PATH]{}, root[MAX_PATH]{}, source[MAX_PATH]{}, state_path[MAX_PATH]{}, gate_path[MAX_PATH]{}, stage_path[MAX_PATH]{};
    const DWORD temp_count = GetTempPathW(MAX_PATH, temp);
    const bool roots = temp_count && temp_count < MAX_PATH && GetTempFileNameW(temp, L"MGL", 0, root) && DeleteFileW(root) && CreateDirectoryW(root, nullptr) &&
        GetTempFileNameW(temp, L"MGM", 0, source) && DeleteFileW(source) && CreateDirectoryW(source, nullptr);
    check(roots, "generation legacy: isolated installation and source roots"); if (!roots) return;
    child(root, L"install.state", state_path); child(root, L"install.lock", gate_path); child(source, L"package.zip", stage_path);
    UpdateGenerationTransaction t{}; UpdateInstallContext install{}; UpdatePackage selected{}; HANDLE members[2]{}; UpdatePackageResult verified{};
    struct Cleanup {
        UpdateGenerationTransaction& t; UpdateInstallContext& install; UpdatePackage& selected; HANDLE* members; UpdatePackageResult& verified;
        const wchar_t* root; const wchar_t* source; const wchar_t* state; const wchar_t* gate; const wchar_t* stage; const u8* old_digest; const u8* new_digest;
        ~Cleanup() noexcept {
            update_package_release(&selected, false); update_package_cleanup_release(&verified); close(&members[0]); close(&members[1]);
            update_install_release(&install); update_generation_release(&t);
            remove_generation(root, old_digest); remove_generation(root, new_digest);
            DeleteFileW(state); DeleteFileW(gate); DeleteFileW(stage); RemoveDirectoryW(root); RemoveDirectoryW(source);
        }
    } cleanup{t, install, selected, members, verified, root, source, state_path, gate_path, stage_path,
        old_generation.descriptor.document_sha256, new_generation.descriptor.document_sha256};

    // The pair's own offer verifies it; a current-format offer or a lone
    // executable under a protocol-1 offer does not.
    UpdateGenerationDirectoryIdentity old_directory{}, old_files[5]{}, new_directory{}, new_files[5]{};
    check(install_generation(root, old_generation, zip, zip_bytes, exe, dll, &old_directory, old_files),
        "generation legacy: fixture lays out a 0.4.x generation with its five fixed files"); if (!old_directory.file) return;
    UpdateGenerationDirectoryIdentity stage_id{};
    HANDLE stage_file = nullptr;
    const bool staged = write_new(stage_path, zip, zip_bytes, &stage_id) &&
        (stage_file = CreateFileW(stage_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) != INVALID_HANDLE_VALUE;
    struct StageOwner { HANDLE& h; ~StageOwner() noexcept { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } } stage_owner{stage_file};
    wchar_t dir[MAX_PATH]{}, member_path[MAX_PATH]{}; generation_directory(root, old_generation.descriptor.document_sha256, dir);
    child(dir, L"MaiCord.exe", member_path); members[0] = CreateFileW(member_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    child(dir, L"MaiCordAccount.dll", member_path); members[1] = CreateFileW(member_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (members[0] == INVALID_HANDLE_VALUE) members[0] = nullptr;
    if (members[1] == INVALID_HANDLE_VALUE) members[1] = nullptr;
    UpdateStage stage{}; stage.file = staged ? stage_file : nullptr; stage.bytes = zip_bytes;
    UpdatePackageDescriptor derived{};
    verified = update_package_verify_installed(&stage, {legacy_text, {old_generation.signature, 64}}, key, workspace, 1000, {scratch, 256}, {members, 2}, &derived);
    check(staged && members[0] && members[1] && verified.ok() && update_package_descriptor_equal(derived, old_generation.descriptor),
        "generation legacy: installed verification derives the migrated pair descriptor from the 0.4.x offer and files");
    char current[512]{}; u8 current_signature[64]{};
    const auto current_text = manifest(current, zip_bytes, zip_digest, 4000999);
    const bool current_refused = sign(current_text, signer, workspace, current_signature) &&
        (verified = update_package_verify_installed(&stage, {current_text, {current_signature, 64}}, key, workspace, 1000, {scratch, 256}, {members, 2}, &derived)).error == UpdatePackageError::zip;
    char lone[512]{}; u8 lone_signature[64]{};
    const auto lone_text = manifest(lone, single_bytes, single_digest, 4000999, kUpdateLegacyProtocol);
    HANDLE lone_stage = nullptr; wchar_t lone_path[MAX_PATH]{}; child(source, L"single.zip", lone_path); UpdateGenerationDirectoryIdentity lone_id{};
    const bool lone_written = write_new(lone_path, single, single_bytes, &lone_id) &&
        (lone_stage = CreateFileW(lone_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)) != INVALID_HANDLE_VALUE;
    UpdateStage lone_staged{}; lone_staged.file = lone_written ? lone_stage : nullptr; lone_staged.bytes = single_bytes;
    const bool lone_refused = lone_written && sign(lone_text, signer, workspace, lone_signature) &&
        update_package_verify_installed(&lone_staged, {lone_text, {lone_signature, 64}}, key, workspace, 1000, {scratch, 256}, {members, 1}, &derived).error == UpdatePackageError::zip;
    if (lone_stage && lone_stage != INVALID_HANDLE_VALUE) CloseHandle(lone_stage);
    DeleteFileW(lone_path);
    check(current_refused && lone_refused, "generation legacy: the account library marks only a protocol-1 pair, and a protocol-1 offer needs both files");
    close(&members[0]); close(&members[1]); close(&stage_file); DeleteFileW(stage_path);

    // The reported failure: a 768-byte install.state opens instead of failing with size.
    UpdateGenerationDirectoryIdentity ids[kUpdateGenerationMaximumAncestors]{}; u32 id_count = 0;
    UpdateGenerationEnrollment enrollment{}; enrollment.root = {root, length(root)}; enrollment.installation = stable.installation;
    enrollment.explicitly_enrolled = true; copy_bytes(enrollment.publisher_inventory_sha256, inventory, 32);
    const bool enrolled = ancestry(root, ids, &id_count); enrollment.ancestors = {ids, id_count};
    const UpdateInstallLimits limits{64 * 1024 * 1024, 1000, kUpdateInstallMaximumRetainedBytes, 0};
    UpdateGenerationState on_disk{}; on_disk.installation = stable.installation; on_disk.sequence = 3; on_disk.active = {old_generation.descriptor, true};
    u8 legacy[kUpdateGenerationLegacyRecordBytes]{}; legacy_record(on_disk, 135, legacy);
    const auto open = [&]() noexcept {
        return update_generation_begin(enrollment, &t).ok() && update_install_initialize(&install, &t, key, workspace, limits).ok();
    };
    const auto close_all = [&]() noexcept { return update_install_release(&install).ok() && update_generation_release(&t).ok(); };
    check(enrolled && write_record(state_path, legacy) && file_bytes(state_path) == kUpdateGenerationLegacyRecordBytes && open() &&
        t.record_present && t.record_legacy && same(t.state, on_disk),
        "generation legacy: a 768-byte v2 install.state opens as the migrated journal");
    check(update_install_recover(&install).ok() && file_bytes(state_path) == kUpdateGenerationLegacyRecordBytes &&
        update_install_select_package(&install, &selected).ok() && selected.descriptor.member_count == 2 && selected.files[0] && selected.files[1] &&
        update_package_descriptor_equal(selected.descriptor, old_generation.descriptor) && update_package_release(&selected, false).ok(),
        "generation legacy: a stable v2 journal is recovered without a write and still selects and pins its 0.4.x pair");
    check(close_all(), "generation legacy: stable owners release");

    // v2 pending (0.4.x crashed after activation): rollback, candidate disposal by
    // its five legacy names, then v3.
    const bool second = install_generation(root, new_generation, zip, zip_bytes, exe, dll, &new_directory, new_files);
    UpdateGenerationState v2_pending{};
    on_disk.sequence = 4; on_disk.phase = UpdateGenerationPhase::pending; on_disk.transaction = transaction;
    on_disk.previous = on_disk.active; on_disk.active = {new_generation.descriptor, true}; copy_value(&v2_pending, on_disk);
    legacy_record(v2_pending, 135, legacy);
    check(second && write_record(state_path, legacy) && open() && t.state.phase == UpdateGenerationPhase::pending && update_install_recover(&install).ok() &&
        t.state.phase == UpdateGenerationPhase::stable && update_package_descriptor_equal(t.state.active.package, old_generation.descriptor) &&
        !t.state.previous.present && !t.state.candidate.present && !t.record_legacy && file_bytes(state_path) == kUpdateGenerationRecordBytes &&
        exists(dir) && close_all(),
        "generation legacy: cold recovery of a v2 pending journal rolls back to the 0.4.x pair, disposes the candidate and writes v3");
    wchar_t new_dir[MAX_PATH]{}; generation_directory(root, new_generation.descriptor.document_sha256, new_dir);
    check(!exists(new_dir) && exists(dir), "generation legacy: only the abandoned candidate generation is removed");

    // v2 rollback with durable intent captured by 0.4.x: the v3 slots must hold
    // the exact v2 file identities or cleanup refuses them.
    const bool third = install_generation(root, new_generation, zip, zip_bytes, exe, dll, &new_directory, new_files);
    UpdateGenerationState v2_rollback{}; copy_value(&v2_rollback, on_disk);
    v2_rollback.sequence = 5; v2_rollback.phase = UpdateGenerationPhase::rollback;
    v2_rollback.active = v2_pending.previous; v2_rollback.previous = {}; v2_rollback.candidate = v2_pending.active;
    v2_rollback.disposal.target = UpdateGenerationDisposalTarget::candidate; v2_rollback.disposal.directory = new_directory; v2_rollback.disposal.file_count = 5;
    for (u32 i = 0; i < 5; ++i) v2_rollback.disposal.files[i] = new_files[i];
    legacy_record(v2_rollback, 135, legacy);
    check(third && write_record(state_path, legacy) && open() && t.state.disposal.target == UpdateGenerationDisposalTarget::candidate &&
        update_install_recover(&install).ok() && t.state.phase == UpdateGenerationPhase::stable && t.state.disposal.target == UpdateGenerationDisposalTarget::none &&
        !exists(new_dir) && file_bytes(state_path) == kUpdateGenerationRecordBytes && close_all(),
        "generation legacy: a v2 candidate disposal intent resumes against the same files and finishes as v3");

    // v2 acknowledged: finish keeps the old pair as previous, which a later
    // update disposes by its five legacy names.
    const bool fourth = install_generation(root, new_generation, zip, zip_bytes, exe, dll, &new_directory, new_files);
    UpdateGenerationState v2_acknowledged{}; copy_value(&v2_acknowledged, v2_pending);
    v2_acknowledged.sequence = 6; v2_acknowledged.phase = UpdateGenerationPhase::acknowledged;
    legacy_record(v2_acknowledged, 135, legacy);
    check(fourth && write_record(state_path, legacy) && open() && update_install_recover(&install).ok() &&
        t.state.phase == UpdateGenerationPhase::stable && update_package_descriptor_equal(t.state.active.package, new_generation.descriptor) &&
        t.state.previous.present && update_package_descriptor_equal(t.state.previous.package, old_generation.descriptor) &&
        file_bytes(state_path) == kUpdateGenerationRecordBytes,
        "generation legacy: a v2 acknowledged journal finishes on the new pair and keeps the old one as previous");
    check(update_install_forget_previous(&install).ok() && !t.state.previous.present && !exists(dir) && exists(new_dir) && close_all(),
        "generation legacy: the previous 0.4.x generation is disposed by its own five files");

    // v2 prepared (0.4.x staged an update, then a current release took over).
    const bool fifth = install_generation(root, old_generation, zip, zip_bytes, exe, dll, &old_directory, old_files);
    UpdateGenerationState v2_prepared{}; v2_prepared.installation = stable.installation; v2_prepared.sequence = 9;
    v2_prepared.phase = UpdateGenerationPhase::prepared; v2_prepared.transaction = transaction;
    v2_prepared.active = {new_generation.descriptor, true}; v2_prepared.candidate = {old_generation.descriptor, true};
    legacy_record(v2_prepared, 135, legacy);
    check(fifth && write_record(state_path, legacy) && open() && update_install_recover(&install).ok() && t.state.phase == UpdateGenerationPhase::prepared &&
        update_install_cancel(&install).ok() && t.state.phase == UpdateGenerationPhase::stable && !t.state.candidate.present && !exists(dir) &&
        update_package_descriptor_equal(t.state.active.package, new_generation.descriptor) && file_bytes(state_path) == kUpdateGenerationRecordBytes && close_all(),
        "generation legacy: a v2 prepared candidate survives recovery and cancels by its legacy names");
}
} // namespace maicord
