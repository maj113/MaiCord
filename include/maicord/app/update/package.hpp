#pragma once

#include "maicord/app/update/prepare.hpp"

namespace maicord {

inline constexpr u32 kUpdatePackageReadBufferBytes = 32 * 1024;
inline constexpr u32 kUpdatePackageMaximumSections = 96;
inline constexpr u64 kUpdatePackageMaximumBytes = 0xfffffffeull;
#if defined(_WIN32)
// A package is a stored ZIP of exactly one member, this AMD64 executable.
inline constexpr Utf8View kUpdatePackageExecutable = utf8_literal("MaiCord.exe");
#else
// A package is the release tarball (packaging/build_tarball.py): one gzip stream of a tar whose directory
// maicord-<X.Y.Z>-linux-x86_64/ holds this x86-64 ELF executable and the flat directories fonts/ and licenses/.
// Everything else in it (the desktop entry, icons, install scripts, the marker) is left alone.
inline constexpr Utf8View kUpdatePackageExecutable = utf8_literal("MaiCord");
inline constexpr Utf8View kUpdatePackageFontDirectory = utf8_literal("fonts");
inline constexpr Utf8View kUpdatePackageLicenseDirectory = utf8_literal("licenses");
// The tar after gunzip, one member, a directory's entries and one file, and the ELF's tables.
inline constexpr u64 kUpdatePackageMaximumTarBytes = 192ull * 1024 * 1024;
inline constexpr u32 kUpdatePackageMaximumEntries = 512;
inline constexpr u32 kUpdatePackageMaximumFiles = 128;
inline constexpr u64 kUpdatePackageMaximumFileBytes = 64ull * 1024 * 1024;
inline constexpr u32 kUpdatePackageMaximumSegments = 32;
inline constexpr u32 kUpdatePackageMaximumNameBytes = 64;
#endif

enum class UpdatePackageError : u8 {
    none, invalid_input, offer, acceptance, source, file, size, read, hash, digest,
    zip, crc, pe, create, write, flush, seal, release,
    // Linux: the gzip stream or the tar inside it is not the producer's layout; the executable is not an x86-64 ELF.
    archive, elf,
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

#if !defined(_WIN32)
// On Linux update_package_extract takes the same inputs and makes the same checks before it creates anything, with
// these differences. `destination` names a directory that must not exist yet: the call creates it and fills it with the
// program and the files of fonts/ and licenses/ (so a swap can move each into the install). The package is read whole
// into memory while its SHA-256 is taken, then gunzipped (gzip_inflate checks the CRC-32 and the length) and walked as a
// tar, so what is verified is exactly what is unpacked. The program is the only file written, flushed, read back and
// hashed again; `output` holds that read-only descriptor. Any failure removes the whole destination. A package fails when
// it is not one gzip member of a tar in the version's directory; when a member is neither a file nor a directory, is
// outside that directory or has a name with an empty, "." or ".." part; when fonts/ or licenses/ holds a nested or oddly
// named file; when a file repeats; or when the program is missing, not executable, or not an x86-64 ELF executable
// image. Every other member (the desktop entry, icons, scripts, the marker) is checked as a name and left alone.
// update_package_release removes the program's name; the stage directory itself goes with remove_stage (service.cpp).
//
// Sharing is advisory on Linux, so before the stage is moved into the install the swap asks whether `stage` still
// holds the file that was verified: the same file as the held descriptor, one link, and the SHA-256 it was given.
bool update_package_intact(const UpdatePackage& package, ConstSlice<wchar_t> stage) noexcept;
#endif

} // namespace maicord
