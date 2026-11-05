#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/platform/crypto/hkdf_sha256.hpp"
#include "maicord/platform/crypto/p256.hpp"

namespace maicord {

// The update protocol: package layout and manifest schema. A client reads only
// updates/p<protocol>/ and accepts manifests whose format is its protocol. A
// release that changes it is also published under the previous protocol, so
// older clients reach it through the newest release they can still install.
//
// Each system has its own manifests and package, signed with the same publisher key:
// the Windows client reads a zip of MaiCord.exe from updates/p2/, the Linux client a
// release tarball from updates/p2/linux/. The schema has no room for either to carry
// the other's entry (a member the reader doesn't know fails the manifest), so a
// system never sees the other's file. What a manifest names is checked against the
// system that reads it: its architecture field and the artifact's name, both signed.
inline constexpr u32 kUpdateProtocol = 2;
inline constexpr Utf8View kUpdateProtocolDirectory = utf8_literal("/updates/p2/");
#if defined(_WIN32)
inline constexpr Utf8View kUpdateManifestArchitecture = utf8_literal("x64");
inline constexpr Utf8View kUpdateArtifactPrefix = utf8_literal("MaiCord-x64-p2-");
#else
// Pages directory of the Linux manifests, below the protocol directory.
inline constexpr Utf8View kUpdateLinuxDirectory = utf8_literal("linux/");
// What the running program is, so which feed it reads: an install from the tarball (the marker beside the executable), or
// an AppImage ($APPIMAGE). Each has its own manifests (architecture linux-x64 or linux-x64-appimage, in linux/ or
// linux/appimage/) and package, signed with the same key. Chosen once at start (update_portable_gate) before any path or
// location is made.
enum class UpdateFlavor : u8 { tarball, appimage };
inline UpdateFlavor g_update_flavor = UpdateFlavor::tarball;
inline constexpr Utf8View kUpdateManifestArchitecture = utf8_literal("linux-x64");
#endif
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

#if !defined(_WIN32)
// The package of a version on Linux, as the file the build's packaging makes and the manifest's `artifact` field must name:
// the release tarball (packaging/build_tarball.py), maicord-<X.Y.Z>-linux-x86_64.tar.gz, or the AppImage
// (packaging/appimage/build_appimage.py), MaiCord-<X.Y.Z>-x86_64.AppImage; X.Y.Z-dev.N for a development build. Written
// into output without a terminator; returns the length, or 0 for a version with no sequence number or when it does not fit.
u32 update_artifact_name(u64 version, Slice<char> output) noexcept;
#endif

// Verification gate only: no network, unpacking, installation or restart.
// The ECDSA key must come from a trusted application key inventory, never the
// document or its download origin. Workers reuse their SHA-256 workspace.
// Output is a copied snapshot, unchanged on any failure. Scratch, document,
// signature, policy, key, workspace and output must not alias each other.
// Persist highest_accepted_version and manifest_sha256 atomically before
// accepting an artifact. Retrying that version requires the identical signed
// document digest in policy; a changed same-version offer is refused.
// the caller supplies trusted wall-clock/freshness policy and pinned origin.
UpdateManifestResult update_manifest_verify(const Utf8View& document, const ConstSlice<u8>& signature,
    const P256Key& trusted_key, KdfSha256Workspace& workspace,
    const UpdateManifestPolicy& policy, const Slice<u32>& structural_scratch,
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
