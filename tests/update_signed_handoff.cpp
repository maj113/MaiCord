#include "maicord/app/update_handoff.hpp"
#include "update_signed_fixture_helpers.hpp"

namespace maicord {
namespace {
using namespace signed_update_fixture;
void metric(const char* label, u64 value) noexcept {
    char out[160]{}, reversed[20]{}; u32 n = 0, digits = 0;
    while (label[n]) { out[n] = label[n]; ++n; } do { reversed[digits++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (digits) out[n++] = reversed[--digits]; out[n++] = '\n'; DWORD got = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, n, &got, nullptr);
}
bool no_launch(const UpdateHandoff& h) noexcept {
    return !h.launched && !h.process && !h.thread && !h.parent && !h.mapping && !h.event && !h.child_mapping && !h.child_event && !h.view && !h.validation;
}
void remove_fixture_generation(UpdateGenerationTransaction& t, const UpdatePackageDescriptor& p) noexcept {
    if (!p.version) return; wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 n = 0;
    for (u32 i = 0; i < update_generation_file_count(p); ++i)
        if (update_generation_path(t, {p.document_sha256,32}, update_generation_file_name(p, i), {path,sizeof(path)/sizeof(wchar_t)}, &n).ok()) {
        HANDLE h = CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE) dispose(&h);
    }
    if (update_generation_path(t, {p.document_sha256,32}, kUpdatePackageExecutable, {path,sizeof(path)/sizeof(wchar_t)}, &n).ok()) {
        while (n && path[n - 1] != L'\\') --n; if (n) path[n - 1] = 0; RemoveDirectoryW(path);
    }
}
bool write_signature(UpdateGenerationTransaction& t, const UpdatePackageDescriptor& p, const u8* signature, HANDLE* owner) noexcept {
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 n = 0;
    if (!update_generation_path(t, {p.document_sha256,32}, update_generation_file_name(p, kUpdateGenerationSignatureSlot),
        {path,sizeof(path)/sizeof(wchar_t)}, &n).ok()) return false;
    *owner = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (*owner == INVALID_HANDLE_VALUE) { *owner = nullptr; return false; }
    return write(*owner, signature, 64) && FlushFileBuffers(*owner) && close(owner);
}
}
void test_update_signed_handoff(void (*check)(bool, const char*) noexcept) noexcept {
#if defined(MAICORD_UPDATE_HANDOFF_TEST_PEER)
    KdfSha256Workspace workspace{}; P256Key signer{}, key{}; u8 scalar[32]{}, point[65]{}, inventory[32]{}, package_digest[32]{}; scalar[31] = 1;
    const bool crypto = kdf_sha256_initialize(&workspace).ok() && p256_import_private(&signer, P256Kind::ecdsa, {scalar,32}).ok() &&
        p256_export_public(signer, {point,65}).ok() && bytes_equal(point, public_point, sizeof(point)) &&
        p256_import_public(&key, P256Kind::ecdsa, {public_point,65}).ok() && update_generation_inventory_fingerprint(key, workspace, {inventory,32}).ok();
    struct CryptoCleanup { KdfSha256Workspace& w; P256Key& s; P256Key& k;
        ~CryptoCleanup() noexcept { p256_release(&k); p256_release(&s); kdf_sha256_release(&w); }
    } crypto_cleanup{workspace, signer, key};
    check(crypto, "signed handoff: fixture-only signing/public inventory independently agrees with peer's public trust point"); if (!crypto) return;
    wchar_t temp[MAX_PATH]{}, root[MAX_PATH]{}, work[MAX_PATH]{}, artifact[MAX_PATH]{}, acceptance[MAX_PATH]{}, acceptance_lock[MAX_PATH + 8]{}, state[MAX_PATH]{}, gate[MAX_PATH]{};
    const DWORD temp_n = GetTempPathW(MAX_PATH, temp);
    const bool roots = temp_n && temp_n < MAX_PATH && GetTempFileNameW(temp, L"MSH", 0, root) && DeleteFileW(root) && CreateDirectoryW(root, nullptr) &&
        GetTempFileNameW(temp, L"MSW", 0, work) && DeleteFileW(work) && CreateDirectoryW(work, nullptr);
    check(roots, "signed handoff: owned synthetic generation and separate work roots created"); if (!roots) return;
    wchar_t notes[MAX_PATH]{}; child(work, L"notes.txt", notes);
    child(work, L"signed-peer.zip", artifact); child(work, L"acceptance.bin", acceptance); child(root, L"install.state", state); child(root, L"install.lock", gate);
    const u32 acceptance_n = length(acceptance); copy_bytes(acceptance_lock, acceptance, acceptance_n * sizeof(wchar_t)); copy_bytes(acceptance_lock + acceptance_n, L".lock", 6 * sizeof(wchar_t));
    UpdateGenerationDirectoryIdentity ancestors[kUpdateGenerationMaximumAncestors]{}; u32 ancestor_count = 0;
    UpdateGenerationEnrollment enrollment{}; enrollment.root = {root,length(root)}; enrollment.installation.bytes[0] = 0x53;
    enrollment.explicitly_enrolled = true; copy_bytes(enrollment.publisher_inventory_sha256, inventory, 32);
    const bool enrolled_ancestors = ancestry(root, ancestors, &ancestor_count); enrollment.ancestors = {ancestors,ancestor_count};
    UpdateGenerationTransaction transaction{}; UpdateInstallContext install{}; UpdatePreparationContext preparation{};
    UpdatePreparationResult prepared{}; UpdateStage stage{}; UpdatePackage selected{}; UpdateHandoff handoff{};
    UpdatePackageDescriptor owned_generations[2]{}; HANDLE package_owner = nullptr, source_owners[2]{}, signature_owner = nullptr, unrelated_owner = nullptr; BCRYPT_HASH_HANDLE hash_owner = nullptr;
    struct Cleanup {
        UpdateGenerationTransaction& t; UpdateInstallContext& i; UpdatePreparationContext& p; UpdatePreparationResult& r; UpdateStage& stage; UpdatePackage& selected; UpdateHandoff& handoff;
        UpdatePackageDescriptor* generations; HANDLE& package; HANDLE* sources; HANDLE& signature; HANDLE& unrelated; BCRYPT_HASH_HANDLE& hash;
        const wchar_t* root; const wchar_t* work; const wchar_t* notes; const wchar_t* artifact; const wchar_t* acceptance; const wchar_t* lock; const wchar_t* state; const wchar_t* gate;
        ~Cleanup() noexcept {
            if (handoff.process) WaitForSingleObject(handoff.process, 5000);
            update_handoff_release(&handoff); close(&signature); close(&unrelated); update_package_release(&selected, false); update_install_release(&i);
            update_stage_cleanup_release(&r.stage); update_stage_release(&stage, true); update_preparation_release(&p);
            if (hash && BCryptDestroyHash(hash) >= 0) hash = nullptr;
            for (u32 n = 0; n < 2; ++n) close(&sources[n]); dispose(&package);
            for (u32 n = 0; n < 2; ++n) remove_fixture_generation(t, generations[n]); DeleteFileW(notes);
            update_generation_release(&t); DeleteFileW(artifact); DeleteFileW(acceptance); DeleteFileW(lock); DeleteFileW(state); DeleteFileW(gate); RemoveDirectoryW(root); RemoveDirectoryW(work);
        }
    } cleanup{transaction,install,preparation,prepared,stage,selected,handoff,owned_generations,package_owner,source_owners,signature_owner,unrelated_owner,hash_owner,
        root,work,notes,artifact,acceptance,acceptance_lock,state,gate};
    HANDLE notes_owner = CreateFileW(notes, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool noted = notes_owner != INVALID_HANDLE_VALUE && write(notes_owner, "release notes", 13) && close(&notes_owner);
    const wchar_t* sources[]{MAICORD_UPDATE_HANDOFF_TEST_PEER, notes}; const char* names[]{"MaiCord.exe", "notes.txt"}; u64 package_bytes = 0;
    const bool packaged = noted && package(artifact, sources, names, 2, &package_owner, source_owners, &package_bytes, package_digest, &hash_owner);
    check(packaged && close(&package_owner) && close(&source_owners[0]) && close(&source_owners[1]),
        "signed handoff: stored ZIP binds controlled peer as MaiCord.exe plus a data member"); if (!packaged || package_owner || source_owners[0] || source_owners[1]) return;
    const UpdateInstallLimits limits{64 * 1024 * 1024, 1000, kUpdateInstallMaximumRetainedBytes, 0};
    check(enrolled_ancestors && update_generation_begin(enrollment, &transaction, true).ok() && update_install_initialize(&install, &transaction, key, workspace, limits).ok(),
        "signed handoff: explicit synthetic provisioning gate binds public publisher inventory"); if (!install.initialized) return;
    char documents[2][512]{}; u8 signatures[2][64]{}; u32 scratch[256]{};
    const auto seed = manifest(documents[0], package_bytes, package_digest, 6); const auto next = manifest(documents[1], package_bytes, package_digest, 7);
    check(sign(seed, signer, workspace, signatures[0]) && sign(next, signer, workspace, signatures[1]), "signed handoff: seed and update offers independently signed");
    UpdateManifestPolicy policy{4,0,100,64 * 1024 * 1024,1000,UpdateChannel::stable};
    UpdatePreparationRequest request{{acceptance,length(acceptance)},{artifact,length(artifact)},{work,length(work)},seed,{signatures[0],64},true};
    prepared = update_prepare_stage(&preparation, request, key, workspace, &policy, {scratch,256}, &stage);
    check(prepared.ok(), "signed handoff: seed receives durable acceptance and retained authenticated stage"); if (!prepared.ok()) return;
    HANDLE retained_files[]{preparation.artifact.file, preparation.acceptance.lock};
    check(update_install_provision_current(&install, &stage, {seed,{signatures[0],64}}, policy, preparation.acceptance, {retained_files,2}).ok(),
        "signed handoff: authenticated seed commits entire generation"); if (!transaction.record_present) return;
    owned_generations[0] = transaction.state.active.package;
    check(update_stage_release(&stage, true).ok() && update_preparation_release(&preparation).ok(), "signed handoff: seed preparation owners released");
    // The newly provisioned seed is now the trusted installed-version floor.
    // A policy retained from before provisioning cannot admit the next install.
    policy.installed_version = transaction.state.active.package.version;
    request.document = next; request.signature = {signatures[1],64}; request.allow_first_use = false;
    prepared = update_prepare_stage(&preparation, request, key, workspace, &policy, {scratch,256}, &stage);
    check(prepared.ok(), "signed handoff: update advances high-water while retaining exact source/stage"); if (!prepared.ok()) return;
    UpdateGenerationIdentity transaction_id{}; transaction_id.bytes[0] = 0xe1;
    retained_files[0] = preparation.artifact.file; retained_files[1] = preparation.acceptance.lock;
    const auto installation = update_install_prepare(&install, &stage, {next,{signatures[1],64}}, policy, preparation.acceptance, transaction_id, {retained_files,2});
    check(installation.ok() &&
        transaction.state.phase == UpdateGenerationPhase::prepared, "signed handoff: complete package/current acceptance commits prepared package");
    if (!installation.ok()) {
        metric("DIAG signed_handoff_prepare_error=", static_cast<u8>(installation.error));
        metric("DIAG signed_handoff_prepare_native=", installation.native_error);
        metric("DIAG signed_handoff_prepare_manifest=", static_cast<u8>(installation.manifest_error));
        metric("DIAG signed_handoff_prepare_generation=", static_cast<u8>(installation.generation.error));
        metric("DIAG signed_handoff_prepare_generation_native=", installation.generation.native_error);
        metric("DIAG signed_handoff_prepare_package=", static_cast<u8>(installation.package.error));
        metric("DIAG signed_handoff_prepare_package_native=", installation.package.native_error);
        metric("DIAG signed_handoff_prepare_package_cleanup=", installation.package.cleanup_error);
        metric("DIAG signed_handoff_prepare_phase=", static_cast<u8>(transaction.state.phase));
        return;
    }
    if (installation.ok()) metric("MEASURE signed_handoff_retained_bytes_bound=", installation.retained_bytes_bound);
    if (transaction.state.phase != UpdateGenerationPhase::prepared) return; owned_generations[1] = transaction.state.candidate.package;
    check(update_install_activate(&install).ok() && transaction.state.phase == UpdateGenerationPhase::pending, "signed handoff: live activation records pending generation");
    UpdateGenerationLaunchAdmission admission{};
    check(update_install_activation_admission(&install, &admission).ok() && update_install_select_activation_package(&install, &selected).ok(),
        "signed handoff: one live admission selects actual verified immutable member pins"); if (!selected.files[0] || !selected.files[1]) return;
    check(update_handoff_launch(&transaction, &install.authentication, selected, false, &admission, &handoff).error == UpdateHandoffError::enrollment && no_launch(handoff),
        "signed handoff: valid signed package still requires explicit trusted caller enrollment");
    check(update_handoff_launch(&transaction, &install.authentication, selected, true, nullptr, &handoff).error == UpdateHandoffError::generation && no_launch(handoff),
        "signed handoff: pending journal alone cannot manufacture live launch admission");
    for (u32 bad = 0; bad < 3; ++bad) { auto wrong = admission;
        if (!bad) wrong.transaction.bytes[0] ^= 1; else if (bad == 1) ++wrong.sequence; else wrong.document_sha256[0] ^= 1;
        const auto result = update_handoff_launch(&transaction, &install.authentication, selected, true, &wrong, &handoff);
        check(result.error == UpdateHandoffError::generation && no_launch(handoff), "signed handoff: stale transaction/sequence/digest admission refuses before process or IPC allocation"); }
    UpdatePackage forged = selected; forged.descriptor.document_sha256[0] ^= 1;
    check(update_handoff_launch(&transaction, &install.authentication, forged, true, &admission, &handoff).error == UpdateHandoffError::generation && no_launch(handoff),
        "signed handoff: unsigned caller descriptor cannot select another target");
    unrelated_owner = CreateFileW(MAICORD_UPDATE_HANDOFF_TEST_PEER, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (unrelated_owner == INVALID_HANDLE_VALUE) unrelated_owner = nullptr;
    check(unrelated_owner != nullptr, "signed handoff: uninstalled equal-byte executable retained solely for file identity refusal");
    if (unrelated_owner) { forged = selected; forged.files[0] = unrelated_owner;
        check(update_handoff_launch(&transaction, &install.authentication, forged, true, &admission, &handoff).error == UpdateHandoffError::target && no_launch(handoff),
            "signed handoff: equal executable bytes outside selected fixed generation cannot substitute its pinned file identity"); close(&unrelated_owner); }
    u8 unsigned_signature[64]{}; copy_bytes(unsigned_signature, signatures[1], 64); unsigned_signature[0] ^= 1;
    const bool corrupted = write_signature(transaction, selected.descriptor, unsigned_signature, &signature_owner);
    check(corrupted, "signed handoff: owned fixture provenance signature changed after selection for fresh-authentication refusal"); if (!corrupted) return;
    const auto unsigned_result = update_handoff_launch(&transaction, &install.authentication, selected, true, &admission, &handoff);
    check(unsigned_result.error == UpdateHandoffError::authenticity && no_launch(handoff), "signed handoff: production launch reauthenticates signed provenance despite valid descriptor and retained member pins");
    const bool restored = write_signature(transaction, selected.descriptor, signatures[1], &signature_owner);
    check(restored, "signed handoff: exact fixture signature restored before authorized launch"); if (!restored) return;
    const auto launched = update_handoff_launch(&transaction, &install.authentication, selected, true, &admission, &handoff);
    check(launched.ok() && launched.outcome == UpdateHandoffOutcome::none && handoff.launched && transaction.gate && selected.files[0] && selected.files[1] &&
        transaction.state.phase == UpdateGenerationPhase::pending, "signed handoff: real production wrapper launches authenticated derived absolute generation path with pins/gate held and no manufactured ack");
    if (!launched.ok()) { metric("DIAG signed_handoff_launch_error=", static_cast<u8>(launched.error)); metric("DIAG signed_handoff_launch_native=", launched.native_error); return; }
    const auto receipt = update_handoff_wait(&handoff, 10000);
    check(receipt.ok() && receipt.outcome == UpdateHandoffOutcome::acknowledged && transaction.state.phase == UpdateGenerationPhase::pending,
        "signed handoff: independently reverified peer admits its linked account API before exact transaction/generation acknowledgement");
    if (!receipt.ok() || receipt.outcome != UpdateHandoffOutcome::acknowledged) { metric("DIAG signed_handoff_wait_error=", static_cast<u8>(receipt.error)); metric("DIAG signed_handoff_wait_outcome=", static_cast<u8>(receipt.outcome)); return; }
    check(update_install_acknowledge(&install, handoff.expected.transaction, {handoff.expected.document_sha256,32}).ok() &&
        update_install_recover(&install).ok() && transaction.state.phase == UpdateGenerationPhase::stable && transaction.state.active.package.version == 7 &&
        preparation.acceptance.state.channels[0].highest_version == 7, "signed handoff: only exact native receipt commits acknowledgement/stable active package and preserves network high-water");
    DWORD exit_code = ~DWORD{0}; const DWORD exited = WaitForSingleObject(handoff.process, 5000);
    check(exited == WAIT_OBJECT_0 && GetExitCodeProcess(handoff.process, &exit_code) && !exit_code, "signed handoff: controlled peer releases account/validation/IPC owners and exits cleanly");
    if (exit_code) metric("DIAG signed_handoff_peer_exit=", exit_code);
    check(update_handoff_release(&handoff).ok() && update_package_release(&selected, false).ok() && update_stage_release(&stage, true).ok() &&
        update_preparation_release(&preparation).ok() && update_install_release(&install).ok(), "signed handoff: production launch and all borrowed/owned preparation/package/coordinator resources explicitly released");
    metric("MEASURE signed_handoff_authenticated_launches=", 1); metric("MEASURE signed_handoff_account_api_admission_receipts=", 1);
#else
    check(false, "signed handoff: root must supply the controlled peer's absolute path");
#endif
}
} // namespace maicord
