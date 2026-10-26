#include "maicord/app/update_prepare.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u8 kPreparedAbcDigest[32]{
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
u32 characters(const wchar_t* text) noexcept { u32 count = 0; while (text[count]) ++count; return count; }
void append(const wchar_t* base, const wchar_t* suffix, wchar_t* output) noexcept {
    u32 i = 0, j = 0;
    while (base[i]) { output[i] = base[i]; ++i; }
    do { output[i + j] = suffix[j]; } while (suffix[j++]);
}
bool write_fixture(const wchar_t* path, const u8* data, u32 count) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, count, &written, nullptr) && written == count;
    return CloseHandle(file) && ok;
}
bool read_abc(HANDLE file) noexcept {
    LARGE_INTEGER zero{};
    u8 bytes[4]{}; DWORD read = 0;
    constexpr u8 abc[]{'a','b','c'};
    return SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) && ReadFile(file, bytes, sizeof(bytes), &read, nullptr) &&
        read == 3 && bytes_equal(bytes, abc, 3);
}
} // namespace

void test_update_prepare(void (*check)(bool, const char*) noexcept) noexcept {
    wchar_t temporary[MAX_PATH]{}, directory[MAX_PATH]{}, record_path[MAX_PATH]{}, artifact_path[MAX_PATH]{};
    wchar_t lock_path[MAX_PATH + 8]{}, new_path[MAX_PATH + 8]{};
    const DWORD temp_count = GetTempPathW(MAX_PATH, temporary);
    const bool reserved = temp_count && temp_count < MAX_PATH && GetTempFileNameW(temporary, L"MUP", 0, directory);
    check(reserved, "prepare: reserve isolated external temporary directory");
    if (!reserved) return;
    const bool created = DeleteFileW(directory) && CreateDirectoryW(directory, nullptr);
    check(created, "prepare: create owned external acceptance and artifact fixture directory");
    if (!created) return;
    append(directory, L"\\acceptance.bin", record_path);
    append(directory, L"\\artifact.bin", artifact_path);
    append(record_path, L".lock", lock_path); append(record_path, L".new", new_path);
    UpdatePreparationContext context{};
    UpdatePreparationResult prepared{};
    struct Cleanup {
        UpdatePreparationContext& context;
        const wchar_t* directory; const wchar_t* record; const wchar_t* artifact;
        const wchar_t* lock; const wchar_t* temporary;
        ~Cleanup() noexcept {
            update_preparation_release(&context);
            DeleteFileW(temporary); DeleteFileW(artifact); DeleteFileW(record); DeleteFileW(lock);
            RemoveDirectoryW(directory);
        }
    } cleanup{context, directory, record_path, artifact_path, lock_path, new_path};
    constexpr u8 abc[]{'a','b','c'}, wrong_bytes[]{'x','b','c'};
    check(write_fixture(artifact_path, abc, 3), "prepare: write known SHA-256 abc artifact fixture");

    KdfSha256Workspace workspace{};
    P256Key signer{}, trusted{};
    struct ReleaseCrypto {
        KdfSha256Workspace& workspace; P256Key& signer; P256Key& trusted;
        ~ReleaseCrypto() noexcept { p256_release(&trusted); p256_release(&signer); kdf_sha256_release(&workspace); }
    } crypto_cleanup{workspace, signer, trusted};
    u8 scalar[32]{}; scalar[31] = 1; // test-only signing key, never a production trust key
    u8 point[65]{}, signature[64]{};
    const bool initialized = kdf_sha256_initialize(&workspace).ok() &&
        p256_import_private(&signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(signer, {point, 65}).ok() && p256_import_public(&trusted, P256Kind::ecdsa, {point, 65}).ok();
    check(initialized, "prepare: test-only signer and independent trusted publisher key initialize");
    if (!initialized) return;
    const auto six = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":6,"issued":90,"expires":200,"bytes":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","artifact":"MaiCord-x64-p2-6.zip"})");
    const auto seven = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":7,"issued":90,"expires":200,"bytes":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","artifact":"MaiCord-x64-p2-7.zip"})");
    const auto preview = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"preview","version":9,"issued":90,"expires":200,"bytes":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","artifact":"MaiCord-x64-p2-9.zip"})");
    auto sign = [&](Utf8View document) noexcept {
        const ConstSlice<u8> parts[]{
            {reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
            {reinterpret_cast<const u8*>(document.data), document.bytes}};
        u8 digest[32]{};
        return sha256(workspace, {parts, 2}, {digest, 32}).ok() && p256_sign_digest(signer, {digest, 32}, {signature, 64}).ok();
    };
    const ConstSlice<wchar_t> storage{record_path, characters(record_path)};
    UpdatePreparationRequest request{storage, {artifact_path, characters(artifact_path)}, six, {signature, 64}, true};
    UpdateManifestPolicy policy{4, 0, 100, 65536, 1000, UpdateChannel::stable};
    u32 scratch[256]{};
    auto run = [&]() noexcept {
        prepared = update_prepare_artifact(&context, request, trusted, workspace, &policy, {scratch, 256});
        return true;
    };
    check(sign(six), "prepare: signed stable abc plan prepared");
    const auto before_policy = policy;
    const auto empty_context = context;
    const auto alias = update_prepare_artifact(&context, request, trusted, workspace, &policy,
        {reinterpret_cast<u32*>(&context), 8});
    auto oversized_path = request;
    oversized_path.artifact_path.count = ~u32{0};
    const auto overflow = update_prepare_artifact(&context, oversized_path, trusted, workspace, &policy, {scratch, 256});
    auto mutable_path_alias = request;
    mutable_path_alias.artifact_path = {reinterpret_cast<const wchar_t*>(&context), 3};
    const auto path_alias = update_prepare_artifact(&context, mutable_path_alias, trusted, workspace, &policy, {scratch, 256});
    check(alias.error == UpdatePreparationError::invalid_input && overflow.error == UpdatePreparationError::invalid_input &&
        path_alias.error == UpdatePreparationError::invalid_input &&
        bytes_equal(&context, &empty_context, sizeof(context)) && bytes_equal(&policy, &before_policy, sizeof(policy)) &&
        GetFileAttributesW(lock_path) == INVALID_FILE_ATTRIBUTES,
        "prepare: scratch/context and path/context aliases and an overflowing path count fail before storage or authority mutation");
    signature[3] ^= 1;
    check(run() && prepared.error == UpdatePreparationError::acceptance &&
        prepared.phase == UpdatePreparationPhase::acceptance_commit &&
        prepared.acceptance.manifest_error == UpdateManifestError::signature && !prepared.acceptance_admitted &&
        context.acceptance.lock && !context.artifact.file && bytes_equal(&policy, &before_policy, sizeof(policy)) &&
        GetFileAttributesW(record_path) == INVALID_FILE_ATTRIBUTES,
        "prepare: bad publisher signature retains only caller-owned lock and admits no record or artifact");
    // A failed release must not discard a real later owner. A real protected
    // event exercises CloseHandle failure without process pseudo-handle values.
    const HANDLE owned_lock = context.acceptance.lock;
    HANDLE protected_owner = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool protected_handle = protected_owner &&
        SetHandleInformation(protected_owner, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE);
    check(protected_handle, "prepare: real owned kernel handle is protected from close for failed-release fixture");
    if (protected_handle) {
        context.artifact.file = protected_owner;
        const auto failed_release = update_preparation_release(&context);
        check(failed_release.error == UpdatePreparationError::artifact &&
            failed_release.phase == UpdatePreparationPhase::release_artifact && context.artifact.file == protected_owner &&
            context.acceptance.lock == owned_lock, "prepare: failed artifact close leaves the acceptance lock owned for explicit retry");
        const bool unprotected = SetHandleInformation(protected_owner, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0) != 0;
        check(unprotected, "prepare: owned fixture protection is explicitly cleared before release retry");
        if (!unprotected) return;
    } else if (protected_owner) CloseHandle(protected_owner);
    check(update_preparation_release(&context).ok() && !context.acceptance.lock,
        "prepare: explicit owner retry closes the still-owned lock and clears context");

    check(sign(six) && run() && prepared.ok() && prepared.acceptance_admitted &&
        context.acceptance.lock && context.artifact.file && context.artifact.version == 6 && context.artifact.bytes == 3 &&
        bytes_equal(context.artifact.sha256, kPreparedAbcDigest, 32) && read_abc(context.artifact.file),
        "prepare: durable current disk admission leads to the retained abc artifact and its expected digest");
    if (!prepared.ok() || !context.artifact.file) return;
    DWORD written = 0;
    const u8 byte = 'x';
    check(!WriteFile(context.artifact.file, &byte, 1, &written, nullptr) && GetLastError() == ERROR_ACCESS_DENIED,
        "prepare: prepared artifact owner has no write capability");
    HANDLE writer = CreateFileW(artifact_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD writer_error = writer == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    UpdateAcceptanceTransaction competitor{};
    const auto competing = update_acceptance_begin(storage, &competitor);
    check(writer == INVALID_HANDLE_VALUE && writer_error == ERROR_SHARING_VIOLATION &&
        competing.error == UpdateAcceptanceError::lock && !competitor.lock,
        "prepare: the artifact denies writers and preparation retains admission serialization until explicit release");
    const auto successful_policy = policy;
    const auto owned_context = context;
    const auto reused = update_prepare_artifact(&context, request, trusted, workspace, &policy, {scratch, 256});
    check(reused.error == UpdatePreparationError::invalid_input &&
        bytes_equal(&context, &owned_context, sizeof(context)) &&
        bytes_equal(&policy, &successful_policy, sizeof(policy)), "prepare: a live context cannot be overwritten or leaked by reuse");
    check(update_preparation_release(&context).ok(), "prepare: explicit release closes admission and artifact owners");

    request.allow_first_use = false;
    policy = {4, 0, 100, 65536, 1000, UpdateChannel::stable};
    check(sign(six) && run() && prepared.ok() && prepared.acceptance_admitted && policy.highest_accepted_version == 6 &&
        read_abc(context.artifact.file) && update_preparation_release(&context).ok(),
        "prepare: a restarted same-offer retry reuses the durable admission of the exact accepted signed bytes");

    request.document = seven;
    check(write_fixture(artifact_path, wrong_bytes, 3) && sign(seven) && run() &&
        prepared.error == UpdatePreparationError::artifact && prepared.phase == UpdatePreparationPhase::artifact_verify &&
        prepared.artifact.error == UpdateArtifactError::digest && prepared.acceptance_admitted &&
        policy.highest_accepted_version == 7 && policy.has_accepted_manifest_digest && !context.artifact.file &&
        context.acceptance.lock,
        "prepare: wrong artifact bytes fail admission while keeping successfully committed rollback advancement");
    UpdateAcceptanceState stored{};
    check(update_acceptance_load(storage, &stored).ok() && stored.channels[0].highest_version == 7 &&
        bytes_equal(stored.channels[0].manifest_sha256, policy.accepted_manifest_sha256, 32) && !stored.channels[1].present,
        "prepare: failed artifact still leaves exact accepted version and document digest durable across restart");
    const auto admitted_policy = policy;
    check(update_preparation_release(&context).ok(), "prepare: failed artifact owner lock is explicitly released");
    char changed[kUpdateManifestMaximumBytes]{};
    copy_bytes(changed, seven.data, seven.bytes); changed[seven.bytes] = ' ';
    request.document = {changed, seven.bytes + 1};
    check(sign(request.document) && run() && prepared.error == UpdatePreparationError::acceptance &&
        prepared.acceptance.manifest_error == UpdateManifestError::rollback && !prepared.acceptance_admitted &&
        bytes_equal(&policy, &admitted_policy, sizeof(policy)) && !context.artifact.file &&
        update_preparation_release(&context).ok(),
        "prepare: changed same-version offer remains rejected after an earlier artifact failure");
    request.document = six;
    check(sign(six) && run() && prepared.error == UpdatePreparationError::acceptance &&
        prepared.acceptance.manifest_error == UpdateManifestError::rollback && !prepared.acceptance_admitted &&
        bytes_equal(&policy, &admitted_policy, sizeof(policy)) && update_preparation_release(&context).ok(),
        "prepare: lower signed offer remains rejected after committed acceptance and failed artifact verification");
    request.document = seven;
    check(write_fixture(artifact_path, abc, 3) && sign(seven) && run() && prepared.ok() &&
        prepared.acceptance_admitted && context.artifact.version == 7 && read_abc(context.artifact.file) &&
        update_preparation_release(&context).ok(),
        "prepare: correcting downloaded bytes permits identical signed-offer retry without lowering acceptance");

    check(DeleteFileW(record_path) && sign(seven) && run() && prepared.error == UpdatePreparationError::acceptance &&
        prepared.phase == UpdatePreparationPhase::acceptance_begin && prepared.acceptance.error == UpdateAcceptanceError::missing &&
        !prepared.acceptance_admitted && !context.acceptance.lock && !context.artifact.file &&
        bytes_equal(&policy, &admitted_policy, sizeof(policy)),
        "prepare: a missing record without first-use provisioning fails closed before artifact open, preserving prior authority");
    u8 record[kUpdateAcceptanceRecordBytes]{};
    check(update_acceptance_encode(stored, {record, sizeof(record)}).ok() && write_fixture(record_path, record, sizeof(record)),
        "prepare: test restores only its previously captured owned acceptance fixture");
    request.document = preview;
    policy = {4, 0, 100, 65536, 1000, UpdateChannel::preview};
    check(sign(preview) && run() && prepared.ok() && prepared.acceptance_admitted && context.artifact.version == 9 &&
        update_acceptance_load(storage, &stored).ok() && stored.channels[0].highest_version == 7 && stored.channels[1].highest_version == 9 &&
        update_preparation_release(&context).ok(),
        "prepare: real signed preview preparation retains stable acceptance independently in one record");
    request.document = seven;
    policy = {4, 0, 100, 65536, 1000, UpdateChannel::stable};
    check(sign(seven) && run() && prepared.ok() && context.artifact.version == 7 &&
        update_acceptance_load(storage, &stored).ok() && stored.channels[0].highest_version == 7 && stored.channels[1].highest_version == 9 &&
        update_preparation_release(&context).ok(),
        "prepare: stable retry after preview preserves both channel histories and releases all owners");
    check(update_preparation_release(&context).ok() &&
        update_preparation_release(nullptr).error == UpdatePreparationError::invalid_input,
        "prepare: explicit context release is idempotent and rejects null owners");
}
} // namespace maicord
