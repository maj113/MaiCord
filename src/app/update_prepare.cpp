#include "maicord/app/update_prepare.hpp"
#include "maicord/core/byte_range.hpp"

#include <Windows.h>

namespace maicord {
namespace {
UpdatePreparationResult failure(UpdatePreparationError error, UpdatePreparationPhase phase) noexcept {
    UpdatePreparationResult result{};
    result.error = error;
    result.phase = phase;
    return result;
}
bool disjoint(ConstSlice<ConstSlice<u8>> ranges) noexcept {
    for (u32 i = 0; i < ranges.count; ++i) {
        if (!byte_range_valid(ranges[i])) return false;
        for (u32 j = 0; j < i; ++j) {
            // Indices 2..4 are borrowed, immutable path views. A parent prefix
            // may share an artifact/acceptance backing buffer without mutation.
            const bool readonly_paths = i >= 2 && i <= 4 && j >= 2 && j <= 4;
            if (!readonly_paths && byte_ranges_overlap(ranges[i], ranges[j])) return false;
        }
    }
    return true;
}
bool fresh(const UpdatePreparationContext& context) noexcept {
    // Explicit release zeroes both owners. Refuse stale caller state as well as
    // live handles so callers cannot accidentally reuse a half-prepared context.
    if (context.acceptance.lock || context.acceptance.path_characters || context.acceptance.record_present ||
        context.artifact.file || context.artifact.bytes || context.artifact.version) return false;
    for (const auto& channel : context.acceptance.state.channels) {
        if (channel.highest_version || channel.present) return false;
        for (auto byte : channel.manifest_sha256) if (byte) return false;
    }
    for (auto character : context.acceptance.path) if (character) return false;
    for (auto byte : context.artifact.sha256) if (byte) return false;
    for (auto byte : context.artifact.manifest_sha256) if (byte) return false;
    return true;
}
void stage_name(const u8* digest, wchar_t* name) noexcept {
    constexpr wchar_t prefix[] = L"maicord-", hex[] = L"0123456789abcdef", suffix[] = L".stage";
    for (u32 i = 0; i < 8; ++i) name[i] = prefix[i];
    for (u32 i = 0; i < 32; ++i) {
        name[8 + i * 2] = hex[digest[i] >> 4];
        name[9 + i * 2] = hex[digest[i] & 15];
    }
    for (u32 i = 0; i < 7; ++i) name[72 + i] = suffix[i];
}
} // namespace

UpdatePreparationResult update_prepare_stage(UpdatePreparationContext* context,
    const UpdatePreparationRequest& request, const P256Key& key, KdfSha256Workspace& workspace,
    UpdateManifestPolicy* policy, Slice<u32> scratch, UpdateStage* output) noexcept {
    if (!context || !policy || !output || !request.acceptance_path.data || !request.acceptance_path.count ||
        request.acceptance_path.count > kUpdateAcceptanceMaximumPathCharacters - 5 ||
        !request.artifact_path.data || !request.artifact_path.count || request.artifact_path.count > kUpdateArtifactMaximumPathCharacters ||
        !request.stage_parent.data || !request.stage_parent.count ||
        request.stage_parent.count > kUpdateArtifactMaximumPathCharacters - kUpdatePreparedStageFilenameCharacters - 1 ||
        !request.document.data || !request.document.bytes || request.document.bytes > kUpdateManifestMaximumBytes ||
        !request.signature.data || request.signature.count != 64 || !scratch.data || !scratch.count ||
        scratch.count > ~u32{0} / sizeof(u32))
        return failure(UpdatePreparationError::invalid_input, UpdatePreparationPhase::input);
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(context), sizeof(*context)},
        {reinterpret_cast<const u8*>(&request), sizeof(request)},
        {reinterpret_cast<const u8*>(request.acceptance_path.data), request.acceptance_path.count * static_cast<u32>(sizeof(wchar_t))},
        {reinterpret_cast<const u8*>(request.artifact_path.data), request.artifact_path.count * static_cast<u32>(sizeof(wchar_t))},
        {reinterpret_cast<const u8*>(request.stage_parent.data), request.stage_parent.count * static_cast<u32>(sizeof(wchar_t))},
        {reinterpret_cast<const u8*>(request.document.data), request.document.bytes}, request.signature,
        {reinterpret_cast<const u8*>(&key), sizeof(key)},
        {reinterpret_cast<const u8*>(&workspace), sizeof(workspace)}, {workspace.storage.data, workspace.storage.capacity},
        {reinterpret_cast<const u8*>(policy), sizeof(*policy)},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))},
        {reinterpret_cast<const u8*>(output), sizeof(*output)}};
    if (!disjoint({ranges, sizeof(ranges) / sizeof(ranges[0])}) || !fresh(*context) || output->file ||
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
        return result;
    }
    wchar_t name[kUpdatePreparedStageFilenameCharacters + 1];
    stage_name(approved.manifest_sha256, name);
    result.stage = update_stage_copy(&context->artifact, approved, request.stage_parent,
        {name, kUpdatePreparedStageFilenameCharacters}, output);
    if (!result.stage.ok()) {
        result.error = UpdatePreparationError::stage;
        result.phase = UpdatePreparationPhase::stage_copy;
    }
    return result;
}

UpdatePreparationResult update_preparation_release(UpdatePreparationContext* context) noexcept {
    if (!context || !byte_range_valid({reinterpret_cast<const u8*>(context), sizeof(*context)}))
        return failure(UpdatePreparationError::invalid_input, UpdatePreparationPhase::input);
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
    SecureZeroMemory(context, sizeof(*context));
    return result;
}
} // namespace maicord
