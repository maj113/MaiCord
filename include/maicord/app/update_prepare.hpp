#pragma once

#include "maicord/app/update_acceptance.hpp"
#include "maicord/app/update_stage.hpp"

namespace maicord {

inline constexpr u32 kUpdatePreparedStageFilenameCharacters = 78;
struct UpdatePreparationRequest {
    ConstSlice<wchar_t> acceptance_path;
    ConstSlice<wchar_t> artifact_path;
    ConstSlice<wchar_t> stage_parent;
    Utf8View document;
    ConstSlice<u8> signature;
    bool allow_first_use = false;
};
// Zero initialize, caller-owned and noncopyable by contract. A failed preparation
// may still hold the acceptance lock or verified artifact. Release explicitly;
// neither this context nor the returned cleanup handles have automatic destructors.
struct UpdatePreparationContext {
    UpdateAcceptanceTransaction acceptance;
    UpdateArtifact artifact;
};
enum class UpdatePreparationError : u8 { none, invalid_input, acceptance, artifact, stage };
enum class UpdatePreparationPhase : u8 {
    none, input, acceptance_begin, acceptance_commit, artifact_verify, stage_copy,
    release_artifact, release_acceptance,
};
// Noncopyable by contract when stage contains cleanup handles. On failure retry
// update_stage_cleanup_release(&result.stage) as well as context release. The
// cleanup handles are disposal capabilities; they never authorize an artifact.
struct UpdatePreparationResult {
    UpdatePreparationError error;
    UpdatePreparationPhase phase;
    bool acceptance_admitted;
    UpdateAcceptanceResult acceptance;
    UpdateArtifactResult artifact;
    UpdateStageResult stage;
    constexpr bool ok() const noexcept { return error == UpdatePreparationError::none; }
};

// Backend only: begin -> signed manifest/durable acceptance -> artifact
// hash -> durable restricted stage. No download, unpack, install or UI.
// Context must be fresh (zero initialized); stage must own no handle. The three
// readonly path views may share backing storage (for example a parent prefix of
// artifact_path). All other inputs, borrowed buffers, context, key/workspace,
// policy, scratch and output are disjoint, including every path against them.
// Paths/parent/first-use enrollment and key/freshness policy are trusted caller
// configuration. Offers supply no trust key, origin or unsigned stage filename.
// Name is derived from the verified signed-domain document digest:
// maicord-<64 lowercase hex digits>.stage (78 characters).
// A failure preserves output stage. If acceptance_admitted=true, the durable
// record and policy keep their advancement even if artifact verification/copy
// fails. Retrying the same signed document is permitted after cleanup and
// releasing/discarding held handles. Existing stages are never overwritten.
// No auto-close: the caller keeps the recoverable context and cleanup handles.
UpdatePreparationResult update_prepare_stage(UpdatePreparationContext* context,
    const UpdatePreparationRequest& request, const P256Key& trusted_key,
    KdfSha256Workspace& workspace, UpdateManifestPolicy* trusted_policy,
    Slice<u32> structural_scratch, UpdateStage* output) noexcept;

// Writes the stage filename for a signed document digest, NUL-terminated, into
// name (kUpdatePreparedStageFilenameCharacters + 1 characters).
void update_prepare_stage_name(const u8* manifest_sha256, wchar_t* name) noexcept;

// Closes artifact before acceptance lock. Stops on a failed release, keeping
// that handle and the acceptance lock handle for retry. Does not release output
// stage or result.stage cleanup handles; those require their respective APIs.
UpdatePreparationResult update_preparation_release(UpdatePreparationContext* context) noexcept;

} // namespace maicord
