#pragma once

#include "maicord/app/update/package.hpp"
#include "maicord/os/file.hpp"

// What both systems' update_package_extract check before they read a byte of the artifact (package.cpp, linux/package.cpp):
// the offer and destination sizes, the current signature and freshness against the live acceptance snapshot, and that
// the downloaded artifact is the one that offer was admitted for.
namespace maicord::package_detail {

inline bool file_owner(HANDLE file) noexcept { return file && file != INVALID_HANDLE_VALUE; }

inline UpdatePackageResult refused(UpdatePackageError error) noexcept {
    UpdatePackageResult result{};
    result.error = error;
    return result;
}

// On success `manifest` holds the verified, admitted offer and the result is ok.
inline UpdatePackageResult admit(UpdateArtifact* source, const UpdatePackageOffer& offer, const P256Key& key,
    KdfSha256Workspace& workspace, const UpdateManifestPolicy& policy, const UpdateAcceptanceTransaction& acceptance,
    Slice<u32> scratch, ConstSlice<wchar_t> destination, UpdateManifest* manifest) noexcept {
    if (!offer.document.bytes || offer.document.bytes > kUpdateManifestMaximumBytes ||
        !destination.count || destination.count > kUpdateArtifactMaximumPathCharacters)
        return refused(UpdatePackageError::invalid_input);
    for (u32 i = 0; i < destination.count; ++i)
        if (!destination[i]) return refused(UpdatePackageError::invalid_input);
    UpdateManifestPolicy captured{};
    if (!update_acceptance_capture_policy(acceptance, policy, &captured).ok()) return refused(UpdatePackageError::acceptance);
    UpdateManifest verified_manifest{};
    const auto verified = update_manifest_verify(offer.document, offer.signature, key, workspace, captured, scratch,
        &verified_manifest);
    if (!verified.ok()) {
        UpdatePackageResult result = refused(UpdatePackageError::offer);
        result.manifest_error = verified.error;
        return result;
    }
    const auto& admitted = acceptance.state.channels[static_cast<u32>(verified_manifest.channel)];
    if (!acceptance.record_present || !admitted.present || admitted.highest_version != verified_manifest.version ||
        !bytes_equal(admitted.manifest_sha256, verified_manifest.manifest_sha256, 32))
        return refused(UpdatePackageError::acceptance);
    if (source->bytes != verified_manifest.artifact_bytes || source->version != verified_manifest.version ||
        !bytes_equal(source->manifest_sha256, verified_manifest.manifest_sha256, 32) ||
        !bytes_equal(source->sha256, verified_manifest.artifact_sha256, 32))
        return refused(UpdatePackageError::source);
    *manifest = verified_manifest;
    return {};
}

} // namespace maicord::package_detail
