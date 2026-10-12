#pragma once

#include "maicord/app/update_manifest.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

inline constexpr u32 kUpdateArtifactMaximumPathCharacters = 1023;
inline constexpr u32 kUpdateArtifactReadBufferBytes = 4096;
// SHA-256 encodes the input bit length in u64. Reject byte counts that would
// overflow that encoding; do not narrow manifest sizes to Win32 DWORD reads.
inline constexpr u64 kUpdateArtifactMaximumBytes = ~u64{0} >> 3;

enum class UpdateArtifactError : u8 {
    none, invalid_input, path, open, file, size, read, hash, digest, release,
};
struct UpdateArtifactResult {
    UpdateArtifactError error;
    DWORD native_error;
    i32 crypto_status;
    constexpr bool ok() const noexcept { return error == UpdateArtifactError::none; }
};

// Caller-owned, noncopyable by contract. Success keeps a read-only disk-file
// handle, positioned at zero, with sharing that denies writers and deletion.
// Staging must consume this handle rather than reopen the path after checking.
// Release before destroying the object. No worker or runtime is held.
struct UpdateArtifact {
    HANDLE file;
    u64 bytes;
    u64 version;
    u8 sha256[32];
    u8 manifest_sha256[32];
};

// The caller must first verify the signed manifest and durably accept its version
// and document digest under update_manifest_verify's policy. This verifies bytes
// against that approved plan; it does not establish trust in a supplied struct.
// Count excludes NUL; accepts ordinary absolute drive/UNC paths, not devices or
// final-file reparse points. Output must own no handle. Inputs/output must not alias.
// Streams a fixed small buffer; no file-size allocation, download or installation.
// Any failure leaves output unchanged and returns no candidate hash/handle.
UpdateArtifactResult update_artifact_verify(ConstSlice<wchar_t> absolute_path,
    const UpdateManifest& approved_manifest, UpdateArtifact* output) noexcept;
UpdateArtifactResult update_artifact_release(UpdateArtifact* artifact) noexcept;

} // namespace maicord
