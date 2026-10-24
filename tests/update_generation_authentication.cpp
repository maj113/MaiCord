#include "maicord/app/update_generation_authentication.hpp"
#include "update_signed_fixture_helpers.hpp"

namespace maicord {
using namespace signed_update_fixture;

void test_update_generation_authentication(void (*check)(bool, const char*) noexcept) noexcept {
    measurement("MEASURE update_package_descriptor_bytes=", sizeof(UpdatePackageDescriptor));
    measurement("MEASURE update_generation_state_bytes=", sizeof(UpdateGenerationState));
    measurement("MEASURE update_generation_transaction_bytes=", sizeof(UpdateGenerationTransaction));
    measurement("MEASURE update_generation_directory_bytes=", sizeof(UpdateGenerationDirectory));
    measurement("MEASURE update_generation_authentication_context_bytes=", sizeof(UpdateGenerationAuthenticationContext));
    KdfSha256Workspace workspace{}; P256Key signer{}, key{}, other{};
    u8 scalar[32]{}; scalar[31] = 1; u8 point[65]{}, inventory[32]{}, expected[32]{}, signature[64]{}, package_digest[32]{};
    UpdateGenerationAuthenticationContext authentication{};
    struct CryptoCleanup { KdfSha256Workspace& w; P256Key& s; P256Key& k; P256Key& o; UpdateGenerationAuthenticationContext& a;
        ~CryptoCleanup() noexcept { update_generation_authentication_release(&a); p256_release(&o); p256_release(&k); p256_release(&s); kdf_sha256_release(&w); }
    } crypto_cleanup{workspace, signer, key, other, authentication};
    const bool initialized = kdf_sha256_initialize(&workspace).ok() && p256_import_private(&signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(signer, {point, 65}).ok() && p256_import_public(&key, P256Kind::ecdsa, {point, 65}).ok();
    check(initialized, "generation auth: test-only signer and independent public publisher key initialize"); if (!initialized) return;
    const ConstSlice<u8> inventory_parts[]{{reinterpret_cast<const u8*>(kUpdatePublisherInventoryDomain.data), kUpdatePublisherInventoryDomain.bytes}, {point, 65}};
    check(sha256(workspace, {inventory_parts, 2}, {expected, 32}).ok() &&
        update_generation_inventory_fingerprint(key, workspace, {inventory, 32}).ok() && bytes_equal(expected, inventory, 32),
        "generation auth: enrolled canonical single public-key inventory domain and SEC1 fingerprint matches");
    check(update_generation_inventory_fingerprint(signer, workspace, {inventory, 32}).error == UpdateGenerationError::invalid_input &&
        bytes_equal(expected, inventory, 32), "generation auth: private fixture signer is never an enrolled production inventory");
    scalar[31] = 2; P256Key other_private{};
    const bool second_key = p256_import_private(&other_private, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(other_private, {point, 65}).ok() && p256_import_public(&other, P256Kind::ecdsa, {point, 65}).ok();
    p256_release(&other_private); check(second_key, "generation auth: wrong public inventory fixture initializes"); if (!second_key) return;

    wchar_t temp[MAX_PATH]{}, root[MAX_PATH]{}, source_root[MAX_PATH]{}, artifact[MAX_PATH]{}, acceptance[MAX_PATH]{}, acceptance_lock[MAX_PATH + 8]{};
    wchar_t state_path[MAX_PATH]{}, gate_path[MAX_PATH]{}, notes[MAX_PATH]{};
    const DWORD temp_count = GetTempPathW(MAX_PATH, temp);
    const bool created = temp_count && temp_count < MAX_PATH && GetTempFileNameW(temp, L"MGA", 0, root) && DeleteFileW(root) && CreateDirectoryW(root, nullptr);
    check(created, "generation auth: isolated explicit fixture installation root"); if (!created) return;
    const bool source_created = GetTempFileNameW(temp, L"MGS", 0, source_root) && DeleteFileW(source_root) && CreateDirectoryW(source_root, nullptr);
    check(source_created, "generation auth: separate owned source/stage/acceptance fixture root");
    if (!source_created) { RemoveDirectoryW(root); return; }
    child(source_root, L"fixture.zip", artifact); child(source_root, L"notes.txt", notes); child(source_root, L"acceptance.bin", acceptance); child(root, L"install.state", state_path); child(root, L"install.lock", gate_path);
    const u32 acceptance_count = length(acceptance); copy_bytes(acceptance_lock, acceptance, acceptance_count * sizeof(wchar_t));
    copy_bytes(acceptance_lock + acceptance_count, L".lock", 6 * sizeof(wchar_t));
    HANDLE sources[2]{}, package_owner = nullptr, provenance[3]{}; bool provenance_owned[3]{};
    UpdatePreparationContext preparation{}; UpdatePreparationResult prepared{}; UpdateStage stage{}; UpdatePackage package{}; UpdatePackageResult extracted{};
    UpdateGenerationTransaction transaction{}; UpdateGenerationDirectory directory{};
    struct FileCleanup {
        HANDLE* sources; HANDLE& package_file; HANDLE* provenance; bool* provenance_owned;
        UpdatePreparationContext& preparation; UpdatePreparationResult& prepared; UpdateStage& stage; UpdatePackage& package; UpdatePackageResult& extracted;
        UpdateGenerationTransaction& transaction; UpdateGenerationDirectory& directory; UpdateGenerationAuthenticationContext& auth;
        const wchar_t* root; const wchar_t* source_root; const wchar_t* artifact; const wchar_t* notes; const wchar_t* acceptance; const wchar_t* acceptance_lock;
        const wchar_t* state; const wchar_t* gate;
        UpdatePackageDescriptor members{}; // set once the members exist
        ~FileCleanup() noexcept {
            update_generation_authentication_release(&auth); update_package_cleanup_release(&extracted); update_package_release(&package, false);
            for (u32 i = 0; i < 3; ++i) if (provenance[i]) dispose(&provenance[i]);
            update_stage_cleanup_release(&prepared.stage); update_stage_release(&stage, true); update_preparation_release(&preparation);
            for (u32 i = 0; i < 2; ++i) if (sources[i]) CloseHandle(sources[i]);
            dispose(&package_file);
            if (directory.directory) {
                for (u32 i = 0; i < update_generation_file_count(members); ++i) if (i >= 3 || provenance_owned[i]) {
                    wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 count = 0;
                    if (update_generation_path(transaction, {directory.document_sha256, 32}, update_generation_file_name(members, i),
                        {path, sizeof(path) / sizeof(wchar_t)}, &count).ok()) DeleteFileW(path);
                }
                update_generation_directory_release(&directory, true);
            }
            update_generation_release(&transaction); DeleteFileW(artifact); DeleteFileW(notes); DeleteFileW(acceptance); DeleteFileW(acceptance_lock);
            DeleteFileW(state); DeleteFileW(gate); RemoveDirectoryW(root); RemoveDirectoryW(source_root);
        }
    } files{sources, package_owner, provenance, provenance_owned, preparation, prepared, stage, package, extracted, transaction, directory, authentication,
        root, source_root, artifact, notes, acceptance, acceptance_lock, state_path, gate_path};
    u64 package_bytes = 0;
    HANDLE notes_file = CreateFileW(notes, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool noted = notes_file != INVALID_HANDLE_VALUE && write(notes_file, "release notes", 13) && close(&notes_file);
    check(noted && real_package(artifact, notes, &package_owner, sources, &package_bytes, package_digest),
        "generation auth: bounded stored ZIP contains actual built MaiCord EXE and a data member without execution");
    if (!package_owner || !package_bytes) return;
    if (!CloseHandle(package_owner)) { check(false, "generation auth: fixture package close"); return; } package_owner = nullptr;
    for (auto& source : sources) if (source) { if (!CloseHandle(source)) return; source = nullptr; }
    char document[512]{}; const auto text = manifest(document, package_bytes, package_digest, 6);
    const ConstSlice<u8> signed_parts[]{{reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(text.data), text.bytes}};
    u8 document_digest[32]{};
    const bool signed_offer = sha256(workspace, {signed_parts, 2}, {document_digest, 32}).ok() &&
        p256_sign_digest(signer, {document_digest, 32}, {signature, 64}).ok();
    UpdateManifestPolicy policy{4, 0, 100, 128 * 1024 * 1024, 1000, UpdateChannel::stable}; u32 scratch[256]{};
    const UpdatePreparationRequest request{{acceptance, length(acceptance)}, {artifact, length(artifact)}, {source_root, length(source_root)}, text, {signature, 64}, true};
    prepared = update_prepare_stage(&preparation, request, key, workspace, &policy, {scratch, 256}, &stage);
    check(signed_offer && prepared.ok(), "generation auth: actual package has current signed durable acceptance and retained prepared stage");
    if (!prepared.ok()) return;
    UpdateGenerationDirectoryIdentity ids[kUpdateGenerationMaximumAncestors]{}; u32 id_count = 0;
    UpdateGenerationEnrollment enrollment{}; enrollment.root = {root, length(root)}; enrollment.installation.bytes[0] = 1;
    enrollment.explicitly_enrolled = true; copy_bytes(enrollment.publisher_inventory_sha256, inventory, 32);
    const bool ancestry_ok = ancestry(root, ids, &id_count); enrollment.ancestors = {ids, id_count};
    check(ancestry_ok && update_generation_begin(enrollment, &transaction, true).ok() &&
        update_generation_directory_open(&transaction, {document_digest, 32}, true, &directory).ok(),
        "generation auth: enrolled inventory and every retained ancestor bind exclusive real-package candidate directory");
    if (!directory.directory) return;
    const UpdatePackageOffer offer{text, {signature, 64}};
    const auto target = update_generation_package_target(&directory);
    extracted = update_package_extract(&stage, offer, key, workspace, policy, preparation.acceptance, {scratch, 256}, target, &package);
    check(extracted.ok() && package.files[0] && package.files[1] && package.descriptor.member_count == 2,
        "generation auth: actual shipping executable and data member validate offline and seal as one candidate");
    if (!extracted.ok()) return; files.members = package.descriptor;
    const auto descriptor = package.descriptor;
    for (u32 i = 0; i < 3; ++i) {
        const auto opened = update_generation_file_open(&directory, update_generation_file_name(descriptor, i), true, &provenance[i]);
        provenance_owned[i] = provenance[i] != nullptr;
        bool ok = opened.ok();
        if (ok) ok = i == 0 ? write(provenance[i], document, text.bytes) : i == 1 ? write(provenance[i], signature, 64) : copy(stage.file, provenance[i], stage.bytes);
        HANDLE sealed = nullptr;
        if (ok) ok = update_generation_file_seal(&directory, &provenance[i], &sealed).ok();
        if (sealed) { if (!provenance[i]) provenance[i] = sealed; else CloseHandle(sealed); }
        check(ok, "generation auth: fixed signed document/signature/outer package provenance stored and flushed"); if (!ok) return;
    }
    check(update_package_release(&package, false).ok(), "generation auth: preserve-release immutable members before fixed-path recovery verification");
    for (auto& owner : provenance) if (owner) { if (!CloseHandle(owner)) return; owner = nullptr; }
    authentication.key = &key; authentication.workspace = &workspace; authentication.maximum_artifact_bytes = policy.maximum_artifact_bytes;
    authentication.maximum_validity_seconds = policy.maximum_validity_seconds; authentication.retained_candidate = &directory;
    const auto authenticator = update_generation_authenticator(&authentication);
    UpdateGenerationState initial{}; initial.installation = enrollment.installation; initial.sequence = 1; initial.active = {descriptor, true};
    check(update_generation_commit(&transaction, initial, authenticator).ok(), "generation auth: real signature/ZIP/PE/member inventory adapter authorizes initial whole-package journal commit");
    UpdatePackageDescriptor selected{};
    check(update_generation_select(&transaction, authenticator, &selected).ok() && update_package_descriptor_equal(selected, descriptor),
        "generation auth: real signed generation select rederives complete installed package descriptor");
    check(update_generation_select_package(&transaction, &authentication, &package).ok() && package.files[0] && package.files[1] &&
        update_package_descriptor_equal(package.descriptor, descriptor), "generation auth: selection retains exact verified member handles through final root validation");
    DWORD prohibited_written = 0; const u8 prohibited_byte = 0;
    wchar_t pinned_path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 pinned_count = 0;
    const bool pinned_path_ok = update_generation_path(transaction, {descriptor.document_sha256, 32}, kUpdatePackageExecutable,
        {pinned_path, sizeof(pinned_path) / sizeof(wchar_t)}, &pinned_count).ok();
    check(package.files[0] && !WriteFile(package.files[0], &prohibited_byte, 1, &prohibited_written, nullptr) && GetLastError() == ERROR_ACCESS_DENIED &&
        pinned_path_ok && !DeleteFileW(pinned_path) && GetLastError() == ERROR_SHARING_VIOLATION,
        "generation auth: selected read-only members pin bytes against write and deletion before launch handoff");
    const bool member_protected = package.files[0] && SetHandleInformation(package.files[0], HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE);
    if (member_protected) {
        const HANDLE retained = package.files[0];
        check(update_package_release(&package, false).error == UpdatePackageError::release && package.files[0] == retained && package.files[1],
            "generation auth: selected package failed close retains every launch owner for retry");
        check(SetHandleInformation(retained, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0) && update_package_release(&package, false).ok(),
            "generation auth: explicit selected package close retry releases immutable launch owners");
    } else check(update_package_release(&package, false).ok(), "generation auth: selected package preserve release");
    policy.now_unix_seconds = 201;
    const auto expired = update_package_extract(&stage, offer, key, workspace, policy, preparation.acceptance, {scratch, 256}, target, &package);
    DWORD native = 0;
    check(expired.error == UpdatePackageError::offer && expired.manifest_error == UpdateManifestError::time &&
        authenticator.verify_installed(authenticator.context, &transaction, descriptor, &native),
        "generation auth: expired offer cannot reinstall as current network offer but signed installed recovery remains authentic");
    authentication.key = &other;
    check(!authenticator.verify_installed(authenticator.context, &transaction, descriptor, &native) &&
        authentication.last_result.error == UpdateGenerationError::authenticity,
        "generation auth: different public inventory cannot authorize a generation under enrolled fingerprint"); authentication.key = &key;
    wchar_t member_path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 member_count = 0;
    const bool path_ok = update_generation_path(transaction, {descriptor.document_sha256, 32}, update_package_member_name(descriptor.members[1]),
        {member_path, sizeof(member_path) / sizeof(wchar_t)}, &member_count).ok();
    HANDLE changed = path_ok ? CreateFileW(member_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr) : INVALID_HANDLE_VALUE;
    bool mutation_ok = false; u8 original = 0;
    if (changed != INVALID_HANDLE_VALUE) { DWORD read = 0; mutation_ok = ReadFile(changed, &original, 1, &read, nullptr) && read == 1 && seek(changed);
        const u8 different = static_cast<u8>(original ^ 1); mutation_ok = mutation_ok && write(changed, &different, 1) && FlushFileBuffers(changed); mutation_ok = CloseHandle(changed) && mutation_ok; }
    check(mutation_ok && !authenticator.verify_installed(authenticator.context, &transaction, descriptor, &native) &&
        authentication.package_result.error == UpdatePackageError::digest,
        "generation auth: installed member mutation rejects checksummed journal even while retained signed outer package is unchanged");
    changed = CreateFileW(member_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const bool restored = changed != INVALID_HANDLE_VALUE && write(changed, &original, 1) && FlushFileBuffers(changed);
    if (changed != INVALID_HANDLE_VALUE) CloseHandle(changed);
    check(restored && authenticator.verify_installed(authenticator.context, &transaction, descriptor, &native),
        "generation auth: restoring exact fixture bytes permits authenticated recovery retry");
    HANDLE protected_owner = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool protected_ok = protected_owner && SetHandleInformation(protected_owner, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE);
    if (protected_ok) {
        authentication.files[0] = protected_owner;
        check(update_generation_authentication_release(&authentication).error == UpdateGenerationError::release && authentication.files[0] == protected_owner,
            "generation auth: failed cleanup close retains exact handle owner for retry");
        const bool cleared = SetHandleInformation(protected_owner, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0) != FALSE;
        check(cleared && update_generation_authentication_release(&authentication).ok() && !authentication.files[0],
            "generation auth: explicit cleanup retry clears only successfully closed owned handle");
    } else if (protected_owner) CloseHandle(protected_owner);
}
} // namespace maicord
