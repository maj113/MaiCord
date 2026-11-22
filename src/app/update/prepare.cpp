#include "maicord/app/update/prepare.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
UpdatePreparationResult failure(UpdatePreparationError error, UpdatePreparationPhase phase) noexcept {
    UpdatePreparationResult result{};
    result.error = error;
    result.phase = phase;
    return result;
}
} // namespace

UpdatePreparationResult update_prepare_artifact(UpdatePreparationContext* context,
    const UpdatePreparationRequest& request, const P256Key& key, KdfSha256Workspace& workspace,
    UpdateManifestPolicy* policy, Slice<u32> scratch) noexcept {
    if (!request.acceptance_path.count || request.acceptance_path.count > kUpdateAcceptanceMaximumPathCharacters - 5 ||
        !request.artifact_path.count || request.artifact_path.count > kUpdateArtifactMaximumPathCharacters ||
        !request.document.bytes || request.document.bytes > kUpdateManifestMaximumBytes ||
        !key.handle || key.kind != P256Kind::ecdsa || !workspace.storage.data || !workspace.storage.capacity ||
        !policy->now_unix_seconds || !policy->maximum_artifact_bytes || !policy->maximum_validity_seconds ||
        policy->channel > UpdateChannel::preview)
        return failure(UpdatePreparationError::invalid_input, UpdatePreparationPhase::input);
    UpdatePreparationResult result{};
    result.acceptance = update_acceptance_begin(request.acceptance_path, &context->acceptance, request.allow_first_use);
    if (!result.acceptance.ok()) {
        result.error = UpdatePreparationError::acceptance;
        result.phase = UpdatePreparationPhase::acceptance_begin;
        return result;
    }
    UpdateManifest approved{};
    result.acceptance = update_acceptance_verify_commit(&context->acceptance, request.document,
        request.signature, key, workspace, policy, scratch, &approved);
    if (!result.acceptance.ok()) {
        result.error = UpdatePreparationError::acceptance;
        result.phase = UpdatePreparationPhase::acceptance_commit;
        return result;
    }
    result.acceptance_admitted = true;
    result.artifact = update_artifact_verify(request.artifact_path, approved, &context->artifact);
    if (!result.artifact.ok()) {
        result.error = UpdatePreparationError::artifact;
        result.phase = UpdatePreparationPhase::artifact_verify;
    }
    return result;
}

UpdatePreparationResult update_preparation_release(UpdatePreparationContext* context) noexcept {
    UpdatePreparationResult result{};
    result.artifact = update_artifact_release(&context->artifact);
    if (!result.artifact.ok()) {
        result.error = UpdatePreparationError::artifact;
        result.phase = UpdatePreparationPhase::release_artifact;
        return result;
    }
    result.acceptance = update_acceptance_release(&context->acceptance);
    if (!result.acceptance.ok()) {
        result.error = UpdatePreparationError::acceptance;
        result.phase = UpdatePreparationPhase::release_acceptance;
        return result;
    }
    secure_clear_bytes(context, sizeof(*context));
    return result;
}
} // namespace maicord
