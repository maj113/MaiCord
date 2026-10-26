#pragma once

#include "maicord/app/update_acceptance.hpp"
#include "maicord/app/update_artifact.hpp"

namespace maicord {

struct UpdatePreparationRequest {
    ConstSlice<wchar_t> acceptance_path;
    ConstSlice<wchar_t> artifact_path;
    Utf8View document;
    ConstSlice<u8> signature;
    bool allow_first_use = false;
};
// Zero initialize, caller-owned and noncopyable by contract. A failed preparation
// may still hold the acceptance lock or verified artifact. Release explicitly;
// neither this context nor the result has automatic destructors.
struct UpdatePreparationContext {
    UpdateAcceptanceTransaction acceptance;
    UpdateArtifact artifact;
};
enum class UpdatePreparationError : u8 { none, invalid_input, acceptance, artifact };
enum class UpdatePreparationPhase : u8 {
    none, input, acceptance_begin, acceptance_commit, artifact_verify,
    release_artifact, release_acceptance,
};
struct UpdatePreparationResult {
    UpdatePreparationError error;
    UpdatePreparationPhase phase;
    bool acceptance_admitted;
    UpdateAcceptanceResult acceptance;
    UpdateArtifactResult artifact;
    constexpr bool ok() const noexcept { return error == UpdatePreparationError::none; }
};

// Backend only: begin -> signed manifest/durable acceptance -> artifact hash.
// No download, unpack, install or UI. Success leaves the acceptance lock and
// the verified artifact (a read-only handle denying writers and deletion) in
// context for update_package_extract.
// Context must be fresh (zero initialized). The two readonly path views may
// share backing storage. All other inputs, borrowed buffers, context,
// key/workspace, policy and scratch are disjoint, including every path
// against them. Paths, first-use provisioning and key/freshness policy are
// trusted caller configuration. Offers supply no trust key or origin.
// If acceptance_admitted=true, the durable record and policy keep their
// advancement even if artifact verification fails. Retrying the same signed
// document is permitted after releasing the context.
// No auto-close: the caller keeps the recoverable context.
UpdatePreparationResult update_prepare_artifact(UpdatePreparationContext* context,
    const UpdatePreparationRequest& request, const P256Key& trusted_key,
    KdfSha256Workspace& workspace, UpdateManifestPolicy* trusted_policy,
    Slice<u32> structural_scratch) noexcept;

// Closes artifact before acceptance lock. Stops on a failed release, keeping
// that handle and the acceptance lock handle for retry.
UpdatePreparationResult update_preparation_release(UpdatePreparationContext* context) noexcept;

} // namespace maicord
