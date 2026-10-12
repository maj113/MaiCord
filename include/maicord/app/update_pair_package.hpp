#pragma once

#include "maicord/app/update_prepare.hpp"

namespace maicord {

inline constexpr u32 kUpdatePairReadBufferBytes = 4096;
inline constexpr u32 kUpdatePairMaximumSections = 96;
inline constexpr u64 kUpdatePairMaximumPackageBytes = 0xfffffffeull;

struct UpdatePairMember { u64 bytes; u8 sha256[32]; };
// Derived from authenticated package bytes, never an independent trust token.
// Serialize fields explicitly (the in-memory layout is not a disk format).
struct UpdatePairDescriptor {
    u64 version;
    u64 package_bytes;
    u8 document_sha256[32];
    u8 package_sha256[32];
    UpdatePairMember members[2]; // MaiCord.exe, MaiCordAccount.dll
    UpdateChannel channel;
    u16 account_ordinal;
};
struct UpdatePair { HANDLE files[2]; UpdatePairDescriptor descriptor; };

// The generation coordinator holds every protected ancestor and the
// exclusive candidate directory. Callbacks must not create directories or choose
// paths from offer data. create uses CREATE_NEW, GENERIC_READ|GENERIC_WRITE|DELETE,
// FILE_SHARE_READ and OPEN_REPARSE_POINT for the fixed member index. On failure
// any returned handle is still owned by the package. validate rechecks directory/root
// identities and the exclusive installation gate before create and commit.
// seal flushes and converts an owned writable handle into a read+DELETE handle
// while preserving writer/deletion exclusion. Success clears *writable. Failure
// keeps every not-yet-closed handle in *writable/*sealed for explicit disposal.
struct UpdatePairTarget {
    void* context;
    bool (*validate)(void*, DWORD*) noexcept;
    bool (*create)(void*, u32, HANDLE*, DWORD*) noexcept;
    bool (*seal)(void*, u32, HANDLE*, HANDLE*, DWORD*) noexcept;
};
enum class UpdatePairError : u8 {
    none, invalid_input, offer, acceptance, stage, size, read, hash, digest,
    zip, crc, pe, target, create, write, flush, seal, release,
};
struct UpdatePairResult {
    UpdatePairError error;
    DWORD native_error;
    i32 crypto_status;
    UpdateManifestError manifest_error;
    DWORD cleanup_error;
    HANDLE cleanup_files[4];
    void* cleanup_hash;
    constexpr bool ok() const noexcept {
        return error == UpdatePairError::none && !cleanup_error && !cleanup_hash;
    }
};
struct UpdatePairOffer {
    Utf8View document;
    ConstSlice<u8> signature;
};

// Revalidates current signature/freshness against the live accepted acceptance
// snapshot. The trusted policy/key originate in enrollment, not offer text.
// Before creating either output: authenticates outer SHA256, checks all ZIP
// metadata/CRC/nonoverlap and offline AMD64 EXE/DLL plus account export ordinal.
// Streams fixed 4096-byte chunks; no heap/archive runtime/DLL execution. Flushes,
// seals and independently hashes/PE-checks both readback files before returning.
// Source cursor is borrowed and serialized. Inputs/buffers/output must be disjoint;
// output is zero initialized, owns no handles, and remains unchanged on failure.
UpdatePairResult update_pair_package_extract(UpdateStage* stage,
    const UpdatePairOffer& offer, const P256Key& key, KdfSha256Workspace& workspace,
    const UpdateManifestPolicy& trusted_policy, const UpdateAcceptanceTransaction& acceptance,
    Slice<u32> structural_scratch, const UpdatePairTarget& target, UpdatePair* output) noexcept;

// Recovery authenticates the installed signed offer without network freshness
// or installed-version floor, then derives the package descriptor anew and compares
// both installed file bytes/PE. Does not promote supplied descriptors.
// Requires protected enrollment/provenance storage and retained generation paths.
UpdatePairResult update_pair_package_verify_installed(UpdateStage* stage,
    const UpdatePairOffer& offer, const P256Key& key, KdfSha256Workspace& workspace,
    u64 maximum_validity_seconds, Slice<u32> structural_scratch, HANDLE executable, HANDLE account,
    UpdatePairDescriptor* output) noexcept;

// Dispose only retained, exclusively created file handles. Stop on a failure and
// keep that handle for retry. No pathname deletion; no unknown-file cleanup.
UpdatePairResult update_pair_release(UpdatePair* pair, bool discard) noexcept;
UpdatePairResult update_pair_cleanup_release(UpdatePairResult* failed) noexcept;

} // namespace maicord
