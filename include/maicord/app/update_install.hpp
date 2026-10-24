#pragma once

#include "maicord/app/update_generation_authentication.hpp"

namespace maicord {

// Occasional caller-owned installation work. The caller enrolls/protects a
// separate work root on the same fixed local volume for source, stage and
// acceptance; none belongs below the canonical generation root. No downloader,
// production key, origin or startup caller.
inline constexpr u64 kUpdateInstallMaximumRetainedBytes = 256ull * 1024 * 1024;
struct UpdateInstallLimits {
    u64 maximum_artifact_bytes;
    u64 maximum_validity_seconds;
    u64 maximum_retained_bytes;
    u64 free_space_reserve_bytes;
};
enum class UpdateInstallError : u8 {
    none, invalid_input, state, enrollment, offer, acceptance, stage, budget,
    disk, generation, extraction, provenance, cleanup, release, remediation,
};
struct UpdateInstallResult {
    UpdateInstallError error;
    DWORD native_error;
    UpdateManifestError manifest_error;
    UpdateGenerationResult generation;
    UpdatePackageResult package; // diagnostic only; cleanup capabilities stay in context
    u64 retained_bytes_bound;
    u64 required_free_bytes;
    constexpr bool ok() const noexcept { return error == UpdateInstallError::none; }
};
// Zero initialize; noncopyable and fields opaque by contract. Transaction,
// public key and exclusively borrowed workspace outlive this object. Release
// before transaction release. Failed operations keep their handles for retry.
struct UpdateInstallContext {
    UpdateGenerationTransaction* transaction;
    UpdateInstallLimits limits;
    UpdateGenerationAuthenticationContext authentication;
    UpdateGenerationDirectory directory;
    UpdatePackage package;
    UpdatePackageResult extraction;
    // By generation slot; member slots hold their files once extraction succeeded.
    HANDLE files[kUpdateGenerationMaximumFiles];
    HANDLE seal_owner;
    UpdateGenerationDirectoryIdentity file_identities[kUpdateGenerationMaximumFiles];
    bool known_files[kUpdateGenerationMaximumFiles];
    bool disposed_files[kUpdateGenerationMaximumFiles];
    bool created_file_owners[kUpdateGenerationMaximumFiles];
    bool disposition_set[kUpdateGenerationMaximumFiles];
    bool disposal_started;
    bool remediation_required;
    bool cleanup_authorized;
    bool initialized;
    bool activation_lease;
    UpdateGenerationIdentity activation_transaction;
    u64 activation_sequence;
    u8 activation_document_sha256[32];
    UpdatePackageDescriptor admitted_package;
    char document[kUpdateManifestMaximumBytes];
    u8 signature[64];
    u32 structural_scratch[256];
};
UpdateInstallResult update_install_initialize(UpdateInstallContext*, UpdateGenerationTransaction*,
    const P256Key&, KdfSha256Workspace&, const UpdateInstallLimits&) noexcept;
// CURRENT signed offer + live durable acceptance + stage, never installed
// authenticity alone. No acceptance mutation. Budget/disk checks precede any
// candidate creation. Output journal changes only after full authenticated copy.
// At most four optional external DISK file handles are measured and
// deduplicated by volume/file identity; stage is always measured automatically.
// Existing offer metadata is budgeted at its fixed 4096-byte cap.
UpdateInstallResult update_install_prepare(UpdateInstallContext*, UpdateStage*, const UpdatePackageOffer&,
    const UpdateManifestPolicy&, const UpdateAcceptanceTransaction&, const UpdateGenerationIdentity&,
    ConstSlice<HANDLE> retained_external_files = {}) noexcept;
// Explicit initial seed provision; caller must have used generation_begin with
// explicit_provision under enrolled protected storage; its successful retained
// transaction must carry provisioning_admitted. Missing ordinary journal
// is never inferred as first use by this coordinator.
UpdateInstallResult update_install_provision_current(UpdateInstallContext*, UpdateStage*, const UpdatePackageOffer&,
    const UpdateManifestPolicy&, const UpdateAcceptanceTransaction&, ConstSlice<HANDLE> retained_external_files = {}) noexcept;
UpdateInstallResult update_install_activate(UpdateInstallContext*) noexcept;
UpdateInstallResult update_install_acknowledge(UpdateInstallContext*, const UpdateGenerationIdentity&,
    ConstSlice<u8> document_sha256) noexcept;
// pending -> authenticated rollback -> candidate cleanup -> stable;
// acknowledged -> stable; prepared remains prepared. Unknown/corrupt orphan
// content requires trusted remediation and is never recursively removed.
// Durable journal intent precedes every deletion of a retained generation.
// Cold recovery accepts absence only for its captured directory/file identities.
UpdateInstallResult update_install_recover(UpdateInstallContext*) noexcept;
UpdateInstallResult update_install_cancel(UpdateInstallContext*) noexcept;
UpdateInstallResult update_install_forget_previous(UpdateInstallContext*) noexcept;
// Stable with a retained previous package: authenticate both and swap them.
UpdateInstallResult update_install_revert(UpdateInstallContext*) noexcept;
// Authenticate/capture and record disposal intent, without deleting a file.
// Useful as the explicit durable boundary for cancellation or shutdown; ordinary
// forget/recover calls perform this step automatically before disposal.
UpdateInstallResult update_install_begin_disposal(UpdateInstallContext*, UpdateGenerationDisposalTarget) noexcept;
// Transfers retained read-only member pins; caller preserve-releases them before
// disposing generations or releasing the installation transaction.
UpdateInstallResult update_install_select_package(UpdateInstallContext*, UpdatePackage*) noexcept;
// One-shot live activation selection, available only after this installer's actual
// pending commit. Cold pending recovery never creates this lease.
UpdateInstallResult update_install_select_activation_package(UpdateInstallContext*, UpdatePackage*) noexcept;
UpdateInstallResult update_install_activation_admission(const UpdateInstallContext*, UpdateGenerationLaunchAdmission*) noexcept;
// Deletes only created files this transaction kept, or freshly authenticated
// journal-referenced disposal files. A failure keeps the handles for retry.
UpdateInstallResult update_install_cleanup(UpdateInstallContext*) noexcept;
// Preserve-release a committed generation; discard an uncommitted failed
// candidate first. Stops at a failed close/disposition, keeping later handles.
UpdateInstallResult update_install_release(UpdateInstallContext*) noexcept;

} // namespace maicord
