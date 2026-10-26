#pragma once

#include "maicord/app/update_prepare.hpp"

namespace maicord {

inline constexpr u32 kUpdatePackageReadBufferBytes = 32 * 1024;
inline constexpr u32 kUpdatePackageMaximumSections = 96;
inline constexpr u64 kUpdatePackageMaximumBytes = 0xfffffffeull;
// A package is a stored ZIP of exactly one member, this AMD64 executable.
inline constexpr Utf8View kUpdatePackageExecutable = utf8_literal("MaiCord.exe");

enum class UpdatePackageError : u8 {
    none, invalid_input, offer, acceptance, source, file, size, read, hash, digest,
    zip, crc, pe, create, write, flush, seal, release,
};
struct UpdatePackageResult {
    UpdatePackageError error;
    DWORD native_error;
    i32 crypto_status;
    UpdateManifestError manifest_error;
    // A failed disposal of the partial output; that file stays on disk as a
    // stale stage, which is never swapped in and is deleted at the next start.
    DWORD cleanup_error;
    constexpr bool ok() const noexcept { return error == UpdatePackageError::none; }
};
struct UpdatePackageOffer {
    Utf8View document;
    ConstSlice<u8> signature;
};
// The verified executable on disk. file has read and DELETE access only, and
// its sharing denies writers and deletion while it is held.
struct UpdatePackage {
    HANDLE file;
    u64 version;
    u64 bytes;
    u8 sha256[32];
};

// Revalidates the current signature and freshness against the live acceptance
// snapshot (the trusted policy and key come from configuration, never the
// offer), then, before creating any output: authenticates the outer SHA-256,
// checks the ZIP metadata, CRC and the single MaiCord.exe member's offline
// AMD64 image. Streams fixed 32 KiB chunks from the verified artifact; no
// heap, archive runtime or code execution. Creates destination with
// CREATE_NEW, flushes it, reduces the handle to read+DELETE and re-hashes and
// PE-checks that file before returning it. Any failure deletes the partial
// output through its handle and leaves output unchanged. Inputs, buffers and
// output must be disjoint; output must own no handle.
UpdatePackageResult update_package_extract(UpdateArtifact* source, const UpdatePackageOffer& offer,
    const P256Key& key, KdfSha256Workspace& workspace, const UpdateManifestPolicy& trusted_policy,
    const UpdateAcceptanceTransaction& acceptance, Slice<u32> structural_scratch,
    ConstSlice<wchar_t> destination, UpdatePackage* output) noexcept;

// discard deletes the file through its handle, never through a pathname.
// A failed disposition or close keeps the handle for a retry.
UpdatePackageResult update_package_release(UpdatePackage* package, bool discard) noexcept;

} // namespace maicord
