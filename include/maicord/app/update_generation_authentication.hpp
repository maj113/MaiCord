#pragma once

#include "maicord/app/update_generation.hpp"

namespace maicord {

inline constexpr Utf8View kUpdatePublisherInventoryDomain = utf8_literal("MaiCord publisher inventory v1\n");
// First enrolled inventory is exactly one public ECDSA P-256 key. Fingerprint is
// SHA256(domain || canonical uncompressed SEC1 public point, 65 bytes). No offers,
// serialized handles, private fixture keys or descriptor checksums establish it.
UpdateGenerationResult update_generation_inventory_fingerprint(const P256Key&,
    KdfSha256Workspace&, Slice<u8> output) noexcept;

// Occasional installer/recovery object, not embedded in the normal account/UI
// runtime. Borrowed key/workspace must remain live, exclusively used and disjoint.
// Context holds failed close/hash handles and must be released before reuse. It
// never closes or deletes the borrowed candidate directory or key/workspace. Preserve-close
// all candidate/provenance READ|DELETE file handles before invoking authenticator;
// their no-delete sharing otherwise prevents reopening read-only slot paths.
struct UpdateGenerationAuthenticationContext {
    const P256Key* key;
    KdfSha256Workspace* workspace;
    u64 maximum_artifact_bytes;
    u64 maximum_validity_seconds;
    UpdateGenerationDirectory* retained_candidate;
    HANDLE files[kUpdateGenerationMaximumFiles]; // by generation slot
    UpdateGenerationDirectory reopened;
    UpdatePackageResult package_result;
    UpdateGenerationResult last_result;
    bool retain_selected_package; // internal one-shot selector mode; never set by callers
    char document[kUpdateManifestMaximumBytes];
    u8 signature[64];
    u32 structural_scratch[256];
};
UpdateGenerationAuthenticator update_generation_authenticator(UpdateGenerationAuthenticationContext*) noexcept;
// Authenticate under the gate and keep the selected read-only members through
// final root/journal validation. Output unchanged on failure; failed
// cleanup handles remain in context. Returned files permit preserve release only
// (no DELETE access), pinning bytes against writes/deletion until launch handoff.
UpdateGenerationResult update_generation_select_package(UpdateGenerationTransaction*,
    UpdateGenerationAuthenticationContext*, UpdatePackage* output) noexcept;
// A coordinator records this only after its live activation commit succeeds.
// It permits one initial launch per journal entry, not recovery.
// Ordinary/cold recovery must keep refusing pending state and roll back instead.
struct UpdateGenerationLaunchAdmission {
    UpdateGenerationIdentity transaction;
    u64 sequence;
    u8 document_sha256[32];
};
UpdateGenerationResult update_generation_select_pending_package_for_launch(UpdateGenerationTransaction*,
    UpdateGenerationAuthenticationContext*, const UpdateGenerationLaunchAdmission&,
    UpdatePackage* output) noexcept;
// Closes retained handles without deleting files. A failed handle is kept for
// retry. Release successful contexts too to erase borrowed data buffers.
UpdateGenerationResult update_generation_authentication_release(UpdateGenerationAuthenticationContext*) noexcept;

} // namespace maicord
