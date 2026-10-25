#pragma once

#include "maicord/app/update_prepare.hpp"

namespace maicord {

inline constexpr u32 kUpdatePackageReadBufferBytes = 4096;
inline constexpr u32 kUpdatePackageMaximumSections = 96;
inline constexpr u64 kUpdatePackageMaximumBytes = 0xfffffffeull;
// A package carries 1 to 4 stored members; the first is the launch target.
inline constexpr u32 kUpdatePackageMaximumMembers = 4;
inline constexpr u32 kUpdatePackageMaximumNameBytes = 32;
inline constexpr Utf8View kUpdatePackageExecutable = utf8_literal("MaiCord.exe");
// A protocol-1 package is exactly MaiCord.exe then this library; a current
// package never carries a member of this name.
inline constexpr Utf8View kUpdatePackageLegacyLibrary = utf8_literal("MaiCordAccount.dll");

struct UpdatePackageMember {
    u64 bytes;
    u8 sha256[32];
    char name[kUpdatePackageMaximumNameBytes];
    u8 name_bytes;
};
// Derived from authenticated package bytes, never an independent trust token.
// Serialize fields explicitly (the in-memory layout is not a disk format).
struct UpdatePackageDescriptor {
    u64 version;
    u64 package_bytes;
    u8 document_sha256[32];
    u8 package_sha256[32];
    UpdatePackageMember members[kUpdatePackageMaximumMembers]; // members[0] is MaiCord.exe
    u32 member_count;
    UpdateChannel channel;
};
struct UpdatePackage { HANDLE files[kUpdatePackageMaximumMembers]; UpdatePackageDescriptor descriptor; };

// A member name is 1-32 ASCII letters, digits, '.', '-' or '_', starting with a
// letter or digit and not ending with '.'; never a device name or one of the
// generation's own provenance names. Members of one package differ ignoring case.
bool update_package_member_name_valid(Utf8View name) noexcept;
Utf8View update_package_member_name(const UpdatePackageMember&) noexcept;
bool update_package_descriptor_equal(const UpdatePackageDescriptor&, const UpdatePackageDescriptor&) noexcept;
// Every field in range, member names valid and distinct, members[0] the
// executable and unused members zero; a zero descriptor is not valid.
bool update_package_descriptor_valid(const UpdatePackageDescriptor&) noexcept;
// A generation installed from a protocol-1 package by a release up to 0.4.x.
bool update_package_descriptor_legacy(const UpdatePackageDescriptor&) noexcept;

// The generation coordinator holds every protected ancestor and the
// exclusive candidate directory. Callbacks must not create directories or choose
// paths from offer data beyond the validated member name. create uses
// CREATE_NEW, GENERIC_READ|GENERIC_WRITE|DELETE, FILE_SHARE_READ and
// OPEN_REPARSE_POINT. On failure any returned handle is still owned by the
// package. validate rechecks directory/root identities and the exclusive
// installation gate before create and commit. seal flushes and converts an
// owned writable handle into a read+DELETE handle while preserving
// writer/deletion exclusion. Success clears *writable. Failure keeps every
// not-yet-closed handle in *writable/*sealed for explicit disposal.
struct UpdatePackageTarget {
    void* context;
    bool (*validate)(void*, DWORD*) noexcept;
    bool (*create)(void*, Utf8View name, HANDLE*, DWORD*) noexcept;
    bool (*seal)(void*, HANDLE*, HANDLE*, DWORD*) noexcept;
};
enum class UpdatePackageError : u8 {
    none, invalid_input, offer, acceptance, stage, size, read, hash, digest,
    zip, crc, pe, target, create, write, flush, seal, release,
};
struct UpdatePackageResult {
    UpdatePackageError error;
    DWORD native_error;
    i32 crypto_status;
    UpdateManifestError manifest_error;
    DWORD cleanup_error;
    HANDLE cleanup_files[kUpdatePackageMaximumMembers * 2];
    void* cleanup_hash;
    constexpr bool ok() const noexcept {
        return error == UpdatePackageError::none && !cleanup_error && !cleanup_hash;
    }
};
struct UpdatePackageOffer {
    Utf8View document;
    ConstSlice<u8> signature;
};

// Revalidates current signature/freshness against the live accepted acceptance
// snapshot. The trusted policy/key originate in enrollment, not offer text.
// Before creating any output: authenticates outer SHA256, checks all ZIP
// metadata/CRC/nonoverlap, member names and offline AMD64 images for .exe and
// .dll members. Streams fixed 4096-byte chunks; no heap/archive runtime/code
// execution. Flushes, seals and independently hashes/PE-checks every readback
// file before returning. Source cursor is borrowed and serialized.
// Inputs/buffers/output must be disjoint; output is zero initialized, owns no
// handles, and remains unchanged on failure.
UpdatePackageResult update_package_extract(UpdateStage* stage,
    const UpdatePackageOffer& offer, const P256Key& key, KdfSha256Workspace& workspace,
    const UpdateManifestPolicy& trusted_policy, const UpdateAcceptanceTransaction& acceptance,
    Slice<u32> structural_scratch, const UpdatePackageTarget& target, UpdatePackage* output) noexcept;

// Recovery authenticates the installed signed offer without network freshness
// or installed-version floor, then derives the package descriptor anew and compares
// every installed member's bytes/PE, given in member order. Does not promote
// supplied descriptors. A protocol-1 offer must describe the legacy pair. Requires protected enrollment/provenance storage and
// retained generation paths.
UpdatePackageResult update_package_verify_installed(UpdateStage* stage,
    const UpdatePackageOffer& offer, const P256Key& key, KdfSha256Workspace& workspace,
    u64 maximum_validity_seconds, Slice<u32> structural_scratch, ConstSlice<HANDLE> members,
    UpdatePackageDescriptor* output) noexcept;

// Dispose only retained, exclusively created file handles. Stop on a failure and
// keep that handle for retry. No pathname deletion; no unknown-file cleanup.
UpdatePackageResult update_package_release(UpdatePackage* package, bool discard) noexcept;
UpdatePackageResult update_package_cleanup_release(UpdatePackageResult* failed) noexcept;

} // namespace maicord
