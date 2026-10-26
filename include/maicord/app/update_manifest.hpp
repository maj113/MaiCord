#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/platform/hkdf_sha256.hpp"
#include "maicord/platform/p256.hpp"

namespace maicord {

// The update protocol: package layout and manifest schema. A client reads only
// updates/p<protocol>/ and accepts manifests whose format is its protocol. A
// release that changes it is also published under the previous protocol, so
// older clients reach it through the newest release they can still install.
inline constexpr u32 kUpdateProtocol = 2;
inline constexpr Utf8View kUpdateProtocolDirectory = utf8_literal("/updates/p2/");
inline constexpr Utf8View kUpdateArtifactPrefix = utf8_literal("MaiCord-x64-p2-");
inline constexpr u32 kUpdateManifestMaximumBytes = 4096;
inline constexpr u32 kUpdateArtifactNameCapacity = 64;
// Sign this prefix followed by the UTF-8 document bytes, with SHA-256
// and P-256/P1363. This prevents reuse of a signature from another protocol.
inline constexpr Utf8View kUpdateManifestSignatureDomain = utf8_literal("MaiCord update manifest v1\n");

enum class UpdateChannel : u8 { stable, preview };
enum class UpdateManifestError : u8 {
    none, invalid_input, signature, json, field, duplicate_field,
    missing_field, unsupported, rollback, time, artifact_size, artifact_name,
};
struct UpdateManifestResult {
    UpdateManifestError error;
    constexpr bool ok() const noexcept { return error == UpdateManifestError::none; }
};
struct UpdateManifestPolicy {
    u64 installed_version;
    u64 highest_accepted_version;
    u64 now_unix_seconds;
    u64 maximum_artifact_bytes;
    u64 maximum_validity_seconds;
    UpdateChannel channel;
    u8 accepted_manifest_sha256[32];
    bool has_accepted_manifest_digest;
};
struct UpdateManifest {
    u64 version;
    u64 issued_unix_seconds;
    u64 expires_unix_seconds;
    u64 artifact_bytes;
    u8 artifact_sha256[32];
    u8 manifest_sha256[32];
    char artifact_name[kUpdateArtifactNameCapacity];
    u32 artifact_name_bytes;
    UpdateChannel channel;
};

// Verification gate only: no network, unpacking, installation or restart.
// The ECDSA key must come from a trusted application key inventory, never the
// document or its download origin. Workers reuse their SHA-256 workspace.
// Output is a copied snapshot, unchanged on any failure. Scratch, document,
// signature, policy, key, workspace and output must not alias each other.
// Persist highest_accepted_version and manifest_sha256 atomically before
// accepting an artifact. Retrying that version requires the identical signed
// document digest in policy; a changed same-version offer is refused.
// the caller supplies trusted wall-clock/freshness policy and pinned origin.
UpdateManifestResult update_manifest_verify(Utf8View document, ConstSlice<u8> signature,
    const P256Key& trusted_key, KdfSha256Workspace& workspace,
    const UpdateManifestPolicy& policy, Slice<u32> structural_scratch,
    UpdateManifest* output) noexcept;

// Authenticates a fetched offer before any durable state: the same signed
// schema and validity interval, without the current clock or update version
// floors. It does not accept a release or modify acceptance state; the
// policy-checked update_manifest_verify still gates admission. Output is
// unchanged on failure.
UpdateManifestResult update_manifest_authenticate(Utf8View document, ConstSlice<u8> signature,
    const P256Key& trusted_key, KdfSha256Workspace& workspace,
    u64 maximum_artifact_bytes, u64 maximum_validity_seconds,
    Slice<u32> structural_scratch, UpdateManifest* output) noexcept;

} // namespace maicord
