#include "maicord/app/update_install.hpp"
#include "maicord/app/account_module_version.hpp"
#include "update_signed_fixture_helpers.hpp"
#include <bcrypt.h>

namespace maicord {
namespace {
using namespace signed_update_fixture;

bool file_hash(HANDLE file, u64 bytes, u8* digest) noexcept {
    if (!seek(file)) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    u8 buffer[kFixtureIoBufferBytes]{}; bool ok = true;
    while (bytes && ok) { DWORD got = 0; const DWORD wanted = static_cast<DWORD>(minimum<u64>(bytes, sizeof(buffer)));
        ok = ReadFile(file, buffer, wanted, &got, nullptr) && got && got <= wanted && BCryptHashData(hash, buffer, got, 0) >= 0;
        bytes -= got;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, 32, 0) >= 0;
    const bool destroyed = BCryptDestroyHash(hash) >= 0; return ok && destroyed;
}
// Fault fixture acquires real authenticated disposal authority and real fixed
// read owners, then retains the same boundaries the coordinator uses. Direct
// owner inspection is test-only; production callers never modify context fields.
bool retain_disposal_fixture(UpdateInstallContext& c, const UpdatePairDescriptor& pair) noexcept {
    if (!update_install_begin_disposal(&c, UpdateGenerationDisposalTarget::previous).ok()) return false;
    if (!c.directory.directory) {
        bool absent = false;
        if (!update_generation_disposal_directory_open(c.transaction, &c.directory, &absent).ok() || absent) return false;
    }
    if (!bytes_equal(c.directory.document_sha256, pair.document_sha256, 32)) return false;
    for (u32 i = 0; i < 5; ++i) {
        if (!update_generation_file_open(&c.directory, static_cast<UpdateGenerationFile>(i), false, &c.files[i]).ok()) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(c.files[i], &info)) return false;
        c.file_identities[i] = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
        c.known_files[i] = true;
    }
    return true;
}
bool dispose_intent_fixture_file(UpdateGenerationTransaction& t, u32 index) noexcept {
    if (index >= 5 || t.state.disposal.target == UpdateGenerationDisposalTarget::none || !update_generation_validate(&t).ok()) return false;
    const auto& ref = t.state.disposal.target == UpdateGenerationDisposalTarget::candidate ? t.state.candidate : t.state.previous;
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 count = 0;
    if (!update_generation_path(t, {ref.pair.document_sha256, 32}, static_cast<UpdateGenerationFile>(index),
        {path, sizeof(path) / sizeof(path[0])}, &count).ok()) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool captured = GetFileInformationByHandle(file, &info) && info.dwVolumeSerialNumber == t.state.disposal.files[index].volume &&
        (static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow) == t.state.disposal.files[index].file &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && info.nNumberOfLinks == 1;
    if (!captured) { CloseHandle(file); return false; }
    return dispose(&file);
}
bool fixture_journal_write(const wchar_t* path, const u8* record) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = write(file, record, kUpdateGenerationRecordBytes) && FlushFileBuffers(file);
    return CloseHandle(file) && written;
}
// Test teardown targets only these fixture-owned fixed generations, without a
// recursive deletion or discovery/adoption of unknown paths.
void remove_fixture_generation(UpdateGenerationTransaction& transaction, const UpdatePairDescriptor& pair) noexcept {
    if (!pair.version) return;
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 count = 0;
    for (u32 i = 0; i < 5; ++i) {
        if (update_generation_path(transaction, {pair.document_sha256, 32}, static_cast<UpdateGenerationFile>(i),
            {path, sizeof(path) / sizeof(path[0])}, &count).ok()) {
            HANDLE h = CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h != INVALID_HANDLE_VALUE) dispose(&h);
        }
    }
    if (update_generation_path(transaction, {pair.document_sha256, 32}, UpdateGenerationFile::executable,
        {path, sizeof(path) / sizeof(path[0])}, &count).ok()) {
        while (count && path[count - 1] != L'\\') --count; if (count) path[count - 1] = 0;
        RemoveDirectoryW(path);
    }
}
} // namespace

void test_update_install(void (*check)(bool, const char*) noexcept) noexcept {
    measurement("MEASURE update_install_context_bytes=", sizeof(UpdateInstallContext));
    measurement("MEASURE update_install_result_bytes=", sizeof(UpdateInstallResult));
    KdfSha256Workspace workspace{}; P256Key signer{}, key{}; u8 scalar[32]{}, point[65]{}, inventory[32]{}, package_digest[32]{};
    scalar[31] = 1;
    const bool crypto = kdf_sha256_initialize(&workspace).ok() && p256_import_private(&signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(signer, {point, 65}).ok() && p256_import_public(&key, P256Kind::ecdsa, {point, 65}).ok() &&
        update_generation_inventory_fingerprint(key, workspace, {inventory, 32}).ok();
    struct CryptoCleanup { KdfSha256Workspace& w; P256Key& s; P256Key& k;
        ~CryptoCleanup() noexcept { p256_release(&k); p256_release(&s); kdf_sha256_release(&w); }
    } crypto_cleanup{workspace, signer, key};
    check(crypto, "install: fixture-only signer and enrolled public inventory initialize"); if (!crypto) return;
    wchar_t temp[MAX_PATH]{}, root[MAX_PATH]{}, work[MAX_PATH]{}, artifact[MAX_PATH]{}, acceptance[MAX_PATH]{}, acceptance_lock[MAX_PATH + 8]{};
    wchar_t state_path[MAX_PATH]{}, gate_path[MAX_PATH]{};
    const DWORD temp_bytes = GetTempPathW(MAX_PATH, temp);
    const bool roots = temp_bytes && temp_bytes < MAX_PATH && GetTempFileNameW(temp, L"MIS", 0, root) && DeleteFileW(root) && CreateDirectoryW(root, nullptr) &&
        GetTempFileNameW(temp, L"MIW", 0, work) && DeleteFileW(work) && CreateDirectoryW(work, nullptr);
    check(roots, "install: isolated enrolled generation and separate trusted source/stage/acceptance roots"); if (!roots) return;
    child(work, L"fixture.zip", artifact); child(work, L"acceptance.bin", acceptance); child(root, L"install.state", state_path); child(root, L"install.lock", gate_path);
    const u32 acceptance_n = length(acceptance); copy_bytes(acceptance_lock, acceptance, acceptance_n * sizeof(wchar_t));
    copy_bytes(acceptance_lock + acceptance_n, L".lock", 6 * sizeof(wchar_t));
    UpdateGenerationDirectoryIdentity ids[kUpdateGenerationMaximumAncestors]{}; u32 id_count = 0;
    UpdateGenerationEnrollment enrollment{}; enrollment.root = {root, length(root)}; enrollment.installation.bytes[0] = 0x43;
    enrollment.explicitly_enrolled = true; copy_bytes(enrollment.publisher_inventory_sha256, inventory, 32);
    const bool identity = ancestry(root, ids, &id_count); enrollment.ancestors = {ids, id_count};
    UpdateGenerationTransaction transaction{}; UpdateInstallContext install{}; UpdatePair selected{}, previous_pins{};
    UpdatePreparationContext preparation{}; UpdatePreparationResult preparation_result{}; UpdateStage stage{};
    UpdatePairDescriptor generations[3]{}; HANDLE package = nullptr;
    struct Cleanup {
        UpdateGenerationTransaction& t; UpdateInstallContext& i; UpdatePair& pair; UpdatePair& previous_pins; UpdatePreparationContext& p;
        UpdatePreparationResult& result; UpdateStage& stage; UpdatePairDescriptor* generations; HANDLE& package;
        const wchar_t* root; const wchar_t* work; const wchar_t* artifact; const wchar_t* acceptance; const wchar_t* lock; const wchar_t* state; const wchar_t* gate;
        ~Cleanup() noexcept {
            update_pair_release(&pair, false); update_pair_release(&previous_pins, false); update_install_release(&i); update_stage_cleanup_release(&result.stage);
            update_stage_release(&stage, true); update_preparation_release(&p);
            dispose(&package);
            for (u32 n = 0; n < 3; ++n) remove_fixture_generation(t, generations[n]);
            update_generation_release(&t); DeleteFileW(artifact); DeleteFileW(acceptance); DeleteFileW(lock); DeleteFileW(state); DeleteFileW(gate);
            RemoveDirectoryW(root); RemoveDirectoryW(work);
        }
    } cleanup{transaction, install, selected, previous_pins, preparation, preparation_result, stage, generations, package,
        root, work, artifact, acceptance, acceptance_lock, state_path, gate_path};
    // These coordinator faults depend on authentic pair/state identities, not
    // shipping payload size. Shipping EXE/DLL integration remains in generation
    // authentication and client tests; the parser shares this exact native ZIP.
    u8 package_data[4096]{};
    const u32 package_bytes = native_pair_package({package_data, sizeof(package_data)});
    const ConstSlice<u8> package_parts[]{{package_data, package_bytes}};
    package = CreateFileW(artifact, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (package == INVALID_HANDLE_VALUE) package = nullptr;
    const bool packaged = package_bytes == 2792 && package && write(package, package_data, package_bytes) && FlushFileBuffers(package) &&
        sha256(workspace, {package_parts, 1}, {package_digest, 32}).ok();
    check(packaged, "install: shared generated native pair package contains exactly 2792 bytes without executing either member"); if (!packaged) return;
    if (!CloseHandle(package)) return; package = nullptr;
    const UpdateInstallLimits limits{64 * 1024 * 1024, 1000, kUpdateInstallMaximumRetainedBytes, 0};
    check(identity && update_generation_begin(enrollment, &transaction, true).ok() &&
        update_install_initialize(&install, &transaction, key, workspace, limits).ok(), "install: coordinator binds retained enrolled root and public inventory");
    if (!install.initialized) return;
    char documents[3][512]{}; u8 signatures[3][64]{}; u32 scratch[256]{};
    const auto seed_text = manifest(documents[0], package_bytes, package_digest, 6);
    const auto next_text = manifest(documents[1], package_bytes, package_digest, 7);
    check(sign(seed_text, signer, workspace, signatures[0]) && sign(next_text, signer, workspace, signatures[1]),
        "install: independent signed release documents bind same generated native pair package");
    UpdateManifestPolicy policy{4, 0, 100, 64 * 1024 * 1024, 1000, UpdateChannel::stable};
    UpdatePreparationRequest request{{acceptance, length(acceptance)}, {artifact, length(artifact)}, {work, length(work)}, seed_text, {signatures[0], 64}, true};
    preparation_result = update_prepare_stage(&preparation, request, key, workspace, &policy, {scratch, 256}, &stage);
    check(preparation_result.ok(), "install: seed signed package receives live durable acceptance and protected retained stage"); if (!preparation_result.ok()) return;
    check(update_install_release(&install).ok() && update_generation_release(&transaction).ok(),
        "install: explicit seed owner releases before testing ordinary missing-journal recovery");
    const auto missing_journal = update_generation_begin(enrollment, &transaction);
    check(!missing_journal.ok() && transaction.gate && !transaction.record_present && !transaction.provisioning_admitted &&
        update_install_initialize(&install, &transaction, key, workspace, limits).ok() &&
        update_install_provision_current(&install, &stage, {seed_text, {signatures[0], 64}}, policy, preparation.acceptance).error == UpdateInstallError::state &&
        !install.directory.created && !transaction.record_present,
        "install: failed ordinary missing-journal begin retains gate but never grants current seed provisioning authority");
    check(update_install_release(&install).ok() && update_generation_release(&transaction).ok() &&
        update_generation_begin(enrollment, &transaction, true).ok() && transaction.provisioning_admitted &&
        update_install_initialize(&install, &transaction, key, workspace, limits).ok(),
        "install: successful explicit provisioning begin supplies separate first-use authority");
    if (!install.initialized || !transaction.provisioning_admitted) return;
    const auto provision = update_install_provision_current(&install, &stage, {seed_text, {signatures[0], 64}}, policy, preparation.acceptance);
    check(provision.ok() && transaction.state.phase == UpdateGenerationPhase::stable && transaction.state.active.pair.version == 6 &&
        transaction.state.active.pair.package_bytes == 2792 && transaction.state.active.pair.members[0].bytes == 1024 &&
        transaction.state.active.pair.members[1].bytes == 1536 && transaction.state.active.pair.account_ordinal == kAccountModuleApiOrdinal &&
        !transaction.provisioning_admitted && provision.retained_bytes_bound && provision.required_free_bytes,
        "install: explicit seed provision publishes authenticated native pair/ordinal and consumes first-use authority after disk/budget preflight");
    if (!provision.ok()) return; generations[0] = transaction.state.active.pair;
    check(update_install_select_pair(&install, &install.pair).error == UpdateInstallError::invalid_input && !install.pair.files[0] &&
        transaction.state.active.pair.version == 6, "install: retained selector output cannot overwrite its opaque installation owner");
    check(update_stage_release(&stage, true).ok() && update_preparation_release(&preparation).ok(), "install: seed work owners explicitly release separately from generation gate");
    policy.installed_version = 6; request.document = next_text; request.signature = {signatures[1], 64}; request.allow_first_use = false;
    preparation_result = update_prepare_stage(&preparation, request, key, workspace, &policy, {scratch, 256}, &stage);
    check(preparation_result.ok(), "install: next offer advances durable accepted high-water under live lock"); if (!preparation_result.ok()) return;
    UpdateGenerationIdentity transaction_id{}; transaction_id.bytes[0] = 0x54;
    const auto accepted = preparation.acceptance.state.channels[0]; const auto stable_sequence = transaction.state.sequence;
    UpdateInstallContext small{}; const UpdateInstallLimits tiny{limits.maximum_artifact_bytes, limits.maximum_validity_seconds, 1, 0};
    check(update_install_initialize(&small, &transaction, key, workspace, tiny).ok() &&
        update_install_prepare(&small, &stage, {next_text, {signatures[1], 64}}, policy, preparation.acceptance, transaction_id).error == UpdateInstallError::budget &&
        !small.directory.created && transaction.state.sequence == stable_sequence && update_install_release(&small).ok(),
        "install: bounded retained-byte preflight rejects before candidate creation and preserves stable journal");
    check(update_install_prepare(&install, &stage, {next_text, {signatures[1], 64}}, policy, preparation.acceptance, transaction_id).ok() &&
        transaction.state.phase == UpdateGenerationPhase::prepared, "install: current acceptance plus extraction and sealed fixed provenance commits prepared pair");
    if (transaction.state.phase != UpdateGenerationPhase::prepared) return; generations[1] = transaction.state.candidate.pair;
    check(update_install_select_pair(&install, &previous_pins).ok() && previous_pins.descriptor.version == 6,
        "install: prepared recovery pins retained old pair, excluding candidate from normal launch");
    check(update_install_activate(&install).ok() && transaction.state.phase == UpdateGenerationPhase::pending &&
        update_install_select_pair(&install, &selected).generation.error == UpdateGenerationError::state,
        "install: live activation commits pending while normal cold selector refuses pending");
    UpdateGenerationLaunchAdmission launch{};
    const auto pending_alias_sequence = transaction.state.sequence;
    check(update_install_activation_admission(&install, reinterpret_cast<UpdateGenerationLaunchAdmission*>(&transaction)).error == UpdateInstallError::invalid_input &&
        transaction.state.sequence == pending_alias_sequence && update_install_select_activation_pair(&install, &install.pair).error == UpdateInstallError::invalid_input &&
        install.activation_lease, "install: admission and pending selector refuse borrowed transaction or opaque owner output aliasing without consuming lease");
    check(update_install_activation_admission(&install, &launch).ok() && workspace.storage.capacity >= sizeof(launch),
        "install: live pending admission fits independently owned workspace alias fault fixture");
    if (workspace.storage.capacity >= sizeof(launch)) {
        copy_bytes(workspace.storage.data, &launch, sizeof(launch));
        const auto* aliased_admission = reinterpret_cast<const UpdateGenerationLaunchAdmission*>(workspace.storage.data);
        check(update_generation_select_pending_pair_for_launch(&transaction, &install.authentication, *aliased_admission, &selected).error ==
            UpdateGenerationError::invalid_input && !selected.files[0] && !selected.files[1] && install.activation_lease &&
            transaction.state.sequence == pending_alias_sequence,
            "install: pending selector refuses admission stored inside borrowed crypto workspace before authentication or publication");
        SecureZeroMemory(workspace.storage.data, workspace.storage.capacity);
    }
    check(update_install_activation_admission(&install, &launch).ok() && update_install_select_activation_pair(&install, &selected).ok() &&
        selected.descriptor.version == 7 && update_install_select_activation_pair(&install, &selected).error == UpdateInstallError::state,
        "install: exact live pending admission provides one-shot retained new-pair pins");
    check(update_pair_release(&selected, false).ok() && update_install_release(&install).ok() && update_generation_release(&transaction).ok() &&
        update_generation_begin(enrollment, &transaction).ok() && update_install_initialize(&install, &transaction, key, workspace, limits).ok(),
        "install: cold reopen starts without a fabricated live activation lease");
    check(update_install_select_activation_pair(&install, &selected).error == UpdateInstallError::state,
        "install: cold pending journal cannot reconstruct a live launch lease");
    UpdateGenerationState rolled_back{};
    check(update_generation_rollback(transaction.state, &rolled_back).ok() &&
        update_generation_commit(&transaction, rolled_back, update_generation_authenticator(&install.authentication)).ok() &&
        update_install_begin_disposal(&install, UpdateGenerationDisposalTarget::candidate).ok() &&
        transaction.state.disposal.target == UpdateGenerationDisposalTarget::candidate,
        "install: cold pending rollback durably authenticates/captures disposal intent before deleting any candidate file");
    if (transaction.state.disposal.target != UpdateGenerationDisposalTarget::candidate) return;
    u8 intent_record[kUpdateGenerationRecordBytes]{};
    check(update_generation_encode(transaction.state, {intent_record, sizeof(intent_record)}).ok() &&
        dispose_intent_fixture_file(transaction, 2) && dispose_intent_fixture_file(transaction, 4),
        "install: crash fixture disposes only captured offer/package identities after durable intent publication");
    wchar_t candidate_path[kUpdateGenerationMaximumRootCharacters + 100]{}, alien_path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 candidate_count = 0;
    if (!update_generation_path(transaction, {generations[1].document_sha256, 32}, UpdateGenerationFile::executable,
        {candidate_path, sizeof(candidate_path) / sizeof(candidate_path[0])}, &candidate_count).ok()) return;
    while (candidate_count && candidate_path[candidate_count - 1] != L'\\') --candidate_count;
    candidate_path[candidate_count - 1] = 0; child(candidate_path, L"alien.fixture", alien_path);
    HANDLE alien = CreateFileW(alien_path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    struct AlienCleanup { HANDLE& file; ~AlienCleanup() noexcept { if (file && file != INVALID_HANDLE_VALUE) dispose(&file); } } alien_cleanup{alien};
    const u8 alien_byte = 0x44;
    check(alien != INVALID_HANDLE_VALUE && write(alien, &alien_byte, 1) && FlushFileBuffers(alien) &&
        update_install_release(&install).ok() && update_generation_release(&transaction).ok() && update_generation_begin(enrollment, &transaction).ok() &&
        update_install_initialize(&install, &transaction, key, workspace, limits).ok() && !update_install_recover(&install).ok() &&
        transaction.state.disposal.target == UpdateGenerationDisposalTarget::candidate && GetFileAttributesW(alien_path) != INVALID_FILE_ATTRIBUTES,
        "install: cold partial disposal rejects bounded unknown entry without adopting it or clearing durable intent");
    if (!install.initialized) return;
    check(update_install_release(&install).ok() && update_generation_release(&transaction).ok(), "install: failed unknown-entry recovery retains disk intent for a new cold owner");
    intent_record[608] ^= 1;
    check(fixture_journal_write(state_path, intent_record) && update_generation_begin(enrollment, &transaction).error == UpdateGenerationError::checksum &&
        transaction.gate && !transaction.record_present && !transaction.provisioning_admitted && GetFileAttributesW(alien_path) != INVALID_FILE_ATTRIBUTES,
        "install: torn durable disposal identity refuses cold journal admission without deleting unknown or surviving files");
    intent_record[608] ^= 1;
    // Checksum refusal retains the exact read owner; release it before the
    // fixture's explicit repair opens this same record for writing.
    const bool repaired_intent = update_generation_release(&transaction).ok() && fixture_journal_write(state_path, intent_record) && dispose(&alien);
    const auto cold_begin = repaired_intent ? update_generation_begin(enrollment, &transaction) : UpdateGenerationResult{};
    const auto cold_initialize = repaired_intent && cold_begin.ok() ? update_install_initialize(&install, &transaction, key, workspace, limits) : UpdateInstallResult{};
    const bool cold_ready = repaired_intent && cold_begin.ok() && cold_initialize.ok();
    const auto cold_recover = cold_ready ? update_install_recover(&install) : UpdateInstallResult{};
    measurement("MEASURE install_cold_intent_repaired=", repaired_intent);
    measurement("MEASURE install_cold_begin_error=", static_cast<usize>(cold_begin.error));
    measurement("MEASURE install_cold_initialize_error=", static_cast<usize>(cold_initialize.error));
    measurement("MEASURE install_cold_recover_error=", static_cast<usize>(cold_recover.error));
    measurement("MEASURE install_cold_recover_generation_error=", static_cast<usize>(cold_recover.generation.error));
    measurement("MEASURE install_cold_recover_native_error=", cold_recover.native_error);
    check(cold_ready && cold_recover.ok() &&
        transaction.state.phase == UpdateGenerationPhase::stable && transaction.state.active.pair.version == 6 &&
        !transaction.state.candidate.present && transaction.state.disposal.target == UpdateGenerationDisposalTarget::none &&
        preparation.acceptance.state.channels[0].highest_version == accepted.highest_version &&
        bytes_equal(preparation.acceptance.state.channels[0].manifest_sha256, accepted.manifest_sha256, 32),
        "install: cold partial disposal resumes without target provenance, authenticates active pair and never lowers acceptance");
    if (!cold_ready || !cold_recover.ok() || transaction.state.phase != UpdateGenerationPhase::stable) return;
    policy.now_unix_seconds = 201;
    const auto expired = update_install_prepare(&install, &stage, {next_text, {signatures[1], 64}}, policy, preparation.acceptance, transaction_id);
    check(expired.error == UpdateInstallError::offer && expired.manifest_error == UpdateManifestError::time && !install.directory.created &&
        update_install_select_pair(&install, &selected).ok() && selected.descriptor.version == 6 && update_pair_release(&selected, false).ok(),
        "install: authentic installed recovery cannot authorize an expired new prepared offer");
    policy.now_unix_seconds = 100;
    check(update_install_prepare(&install, &stage, {next_text, {signatures[1], 64}}, policy, preparation.acceptance, transaction_id).ok() &&
        update_install_activate(&install).ok(), "install: identical admitted signed offer retries after complete rollback cleanup");
    UpdateGenerationIdentity wrong = transaction_id; wrong.bytes[1] = 1; const auto pending_sequence = transaction.state.sequence;
    check(!update_install_acknowledge(&install, wrong, {generations[1].document_sha256, 32}).ok() && transaction.state.sequence == pending_sequence &&
        update_install_acknowledge(&install, transaction_id, {generations[1].document_sha256, 32}).ok() &&
        update_install_recover(&install).ok() && transaction.state.phase == UpdateGenerationPhase::stable && transaction.state.active.pair.version == 7 &&
        transaction.state.previous.present,
        "install: exact startup acknowledgement completes stable pair while previous remains available");
    if (!transaction.state.previous.present) return;
    const auto pinned_disposal = update_install_forget_previous(&install);
    check(!pinned_disposal.ok() && pinned_disposal.native_error == ERROR_SHARING_VIOLATION &&
        transaction.state.disposal.target == UpdateGenerationDisposalTarget::previous && transaction.state.previous.present &&
        previous_pins.files[0] && previous_pins.files[1] && transaction.state.active.pair.version == 7,
        "install: retained previous-pair launch pins block deletion while durable intent and authenticated active pair remain selected");
    check(update_pair_release(&previous_pins, false).ok() && update_install_release(&install).ok() &&
        update_install_initialize(&install, &transaction, key, workspace, limits).ok(),
        "install: releasing external pair pins and process-local cleanup owner permits a fresh exact-intent retry");
    // Retain the final read close to fail before disposal. The EXE's earlier
    // captured read owner closes; replace that file while its expected identity
    // remains frozen, then prove retry never widens authority to the replacement.
    check(retain_disposal_fixture(install, transaction.state.previous.pair), "install: cleanup fault fixture retains genuine signed previous-generation authority");
    if (!install.files[4]) return;
    const HANDLE blocked_close = install.files[4];
    const bool close_blocked = SetHandleInformation(blocked_close, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE) != FALSE;
    check(close_blocked && !update_install_forget_previous(&install).ok() && install.files[4] == blocked_close,
        "install: authenticated previous disposal retains failed preserve-close owner before deleting files");
    wchar_t member[kUpdateGenerationMaximumRootCharacters + 100]{}, saved[MAX_PATH]{}; u32 member_count = 0;
    child(work, L"retained-original.exe", saved);
    const bool member_path = update_generation_path(transaction, {generations[0].document_sha256, 32}, UpdateGenerationFile::executable,
        {member, sizeof(member) / sizeof(member[0])}, &member_count).ok();
    const bool moved = member_path && MoveFileExW(member, saved, 0);
    HANDLE original = moved ? CreateFileW(saved, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr) : INVALID_HANDLE_VALUE;
    HANDLE replacement = moved ? CreateFileW(member, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr) : INVALID_HANDLE_VALUE;
    const bool replaced = original != INVALID_HANDLE_VALUE && replacement != INVALID_HANDLE_VALUE &&
        copy(original, replacement, generations[0].members[0].bytes) && FlushFileBuffers(replacement);
    if (original != INVALID_HANDLE_VALUE) CloseHandle(original);
    if (replacement != INVALID_HANDLE_VALUE) CloseHandle(replacement);
    check(replaced && SetHandleInformation(blocked_close, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0) &&
        update_install_forget_previous(&install).error == UpdateInstallError::remediation && install.remediation_required &&
        update_install_forget_previous(&install).error == UpdateInstallError::remediation && GetFileAttributesW(member) != INVALID_FILE_ATTRIBUTES,
        "install: changed captured file identity permanently refuses cleanup retry and leaves replacement intact");
    check(update_install_release(&install).error == UpdateInstallError::remediation && GetFileAttributesW(member) != INVALID_FILE_ATTRIBUTES,
        "install: remediation release preserve-closes replacement and never adopts or deletes it");
    replacement = CreateFileW(member, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const bool restored_previous = replacement != INVALID_HANDLE_VALUE && dispose(&replacement) && MoveFileExW(saved, member, 0);
    check(restored_previous && update_install_initialize(&install, &transaction, key, workspace, limits).ok(),
        "install: explicit fixture remediation restores original publisher bytes and fresh authentication owner");
    if (!restored_previous || !install.initialized) return;
    // A fixture-owned record collision fails the actual native journal commit
    // after successful signed previous cleanup. Durable intent must survive a
    // cold restart without authenticating now-disposed target provenance.
    wchar_t temporary_record[MAX_PATH]{}; child(root, L"install.state.new", temporary_record);
    HANDLE collision = CreateFileW(temporary_record, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const u8 blocker = 0x7f;
    const bool collided = collision != INVALID_HANDLE_VALUE && write(collision, &blocker, 1) && FlushFileBuffers(collision);
    const auto cleanup_sequence = transaction.state.sequence;
    const auto commit_failed = update_install_forget_previous(&install);
    check(collided && !commit_failed.ok() && transaction.state.disposal.target == UpdateGenerationDisposalTarget::previous &&
        transaction.state.sequence == cleanup_sequence && transaction.state.previous.present && !install.directory.directory,
        "install: failed journal publication after all files/directory are disposed preserves exact durable intent");
    check(collision != INVALID_HANDLE_VALUE && dispose(&collision) && update_install_release(&install).ok() && update_generation_release(&transaction).ok(),
        "install: completed-disposal crash fixture releases only process owners while leaving durable intent selected");
    wchar_t replacement_directory_path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 replacement_count = 0;
    // The generation path formatter needs its trusted root; reopen the intent
    // journal before creating this fixture-owned replacement directory.
    check(update_generation_begin(enrollment, &transaction).ok() &&
        update_generation_path(transaction, {generations[0].document_sha256, 32}, UpdateGenerationFile::executable,
            {replacement_directory_path, sizeof(replacement_directory_path) / sizeof(replacement_directory_path[0])}, &replacement_count).ok(),
        "install: cold complete-disposal journal retains exact target digest and directory identity");
    while (replacement_count && replacement_directory_path[replacement_count - 1] != L'\\') --replacement_count;
    if (!replacement_count) return;
    replacement_directory_path[replacement_count - 1] = 0;
    const bool replacement_created = CreateDirectoryW(replacement_directory_path, nullptr) != FALSE;
    HANDLE replacement_directory = replacement_created ? CreateFileW(replacement_directory_path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) : INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION replacement_info{};
    const bool replacement_captured = replacement_directory != INVALID_HANDLE_VALUE && GetFileInformationByHandle(replacement_directory, &replacement_info);
    const UpdateGenerationDirectoryIdentity replacement_identity{replacement_info.dwVolumeSerialNumber,
        static_cast<u64>(replacement_info.nFileIndexHigh) << 32 | replacement_info.nFileIndexLow};
    check(replacement_captured && replacement_identity.file != transaction.state.disposal.directory.file &&
        update_install_initialize(&install, &transaction, key, workspace, limits).ok() &&
        update_install_recover(&install).error == UpdateInstallError::remediation &&
        transaction.state.disposal.target == UpdateGenerationDisposalTarget::previous && GetFileAttributesW(replacement_directory_path) != INVALID_FILE_ATTRIBUTES,
        "install: cold absence-tolerant disposal refuses a replacement generation directory and preserves it intact");
    check(update_install_release(&install).error == UpdateInstallError::remediation && CloseHandle(replacement_directory),
        "install: replacement refusal preserve-closes only observed directory owners before explicit fixture repair");
    replacement_directory = CreateFileW(replacement_directory_path, FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION repair_info{}; FILE_DISPOSITION_INFO repair_disposition{TRUE};
    const bool exact_repair = replacement_directory != INVALID_HANDLE_VALUE && GetFileInformationByHandle(replacement_directory, &repair_info) &&
        repair_info.dwVolumeSerialNumber == replacement_identity.volume &&
        (static_cast<u64>(repair_info.nFileIndexHigh) << 32 | repair_info.nFileIndexLow) == replacement_identity.file &&
        SetFileInformationByHandle(replacement_directory, FileDispositionInfo, &repair_disposition, sizeof(repair_disposition));
    if (replacement_directory != INVALID_HANDLE_VALUE) CloseHandle(replacement_directory);
    check(exact_repair && update_generation_release(&transaction).ok() &&
        update_generation_begin(enrollment, &transaction).ok() && update_install_initialize(&install, &transaction, key, workspace, limits).ok() &&
        update_install_recover(&install).ok() && !transaction.state.previous.present && transaction.state.disposal.target == UpdateGenerationDisposalTarget::none,
        "install: cold after complete physical disposal finalizes previous-forget using durable intent without missing-file authentication");
    check(update_stage_release(&stage, true).ok() && update_preparation_release(&preparation).ok(), "install: admitted retry stage and acceptance owners release");

    // Authenticated malformed ZIP passes outer admission, then extraction fails
    // before pair creation. Cleanup must retain failed directory-close ownership.
    package = CreateFileW(artifact, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const u8 changed = 0;
    const bool corrupted = package != INVALID_HANDLE_VALUE && seek(package) && write(package, &changed, 1) && FlushFileBuffers(package) &&
        file_hash(package, package_bytes, package_digest);
    if (package == INVALID_HANDLE_VALUE) package = nullptr;
    check(corrupted, "install: fixture signs malformed outer package to exercise post-admission extraction failure"); if (!corrupted) return;
    if (!CloseHandle(package)) return; package = nullptr;
    const auto bad_text = manifest(documents[2], package_bytes, package_digest, 8);
    if (!sign(bad_text, signer, workspace, signatures[2])) return;
    policy.installed_version = 7; request.document = bad_text; request.signature = {signatures[2], 64};
    preparation_result = update_prepare_stage(&preparation, request, key, workspace, &policy, {scratch, 256}, &stage);
    check(preparation_result.ok(), "install: failed install may retain a higher independently admitted network release"); if (!preparation_result.ok()) return;
    const auto before_failure = transaction.state.sequence;
    const auto failed = update_install_prepare(&install, &stage, {bad_text, {signatures[2], 64}}, policy, preparation.acceptance, transaction_id);
    check(failed.error == UpdateInstallError::extraction && install.directory.created && transaction.state.sequence == before_failure &&
        transaction.state.active.pair.version == 7 && preparation.acceptance.state.channels[0].highest_version == 8,
        "install: failed preparation keeps stable pair/journal atomic and accepted high-water advanced");
    if (!install.directory.directory) return;
    const HANDLE retained = install.directory.directory;
    const bool protected_owner = SetHandleInformation(retained, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE) != FALSE;
    check(protected_owner && !update_install_cleanup(&install).ok() && install.directory.directory == retained && transaction.state.sequence == before_failure,
        "install: failed cleanup close retains exact owned directory and unchanged journal for retry");
    if (protected_owner) {
        check(SetHandleInformation(retained, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0) && update_install_cleanup(&install).ok() &&
            !install.directory.created && !install.directory.directory && transaction.state.sequence == before_failure,
            "install: cleanup retry disposes only retained candidate without changing committed stable generation");
    }
    check(update_stage_release(&stage, true).ok() && update_preparation_release(&preparation).ok(), "install: malformed release work owners clean up separately");
    UpdateAcceptanceState high_water{};
    check(update_acceptance_load({acceptance, length(acceptance)}, &high_water).ok() && high_water.channels[0].highest_version == 8 &&
        update_install_select_pair(&install, &selected).ok() && selected.descriptor.version == 7,
        "install: disk high-water survives rollback and failed install while authentic active pair remains selectable");
}
} // namespace maicord
