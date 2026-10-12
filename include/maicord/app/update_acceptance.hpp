#pragma once

#include "maicord/app/update_manifest.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

inline constexpr u32 kUpdateAcceptanceRecordBytes = 128;
inline constexpr u32 kUpdateAcceptanceMaximumPathCharacters = 1023;
struct UpdateAcceptanceChannel {
    u64 highest_version;
    u8 manifest_sha256[32];
    bool present;
};
struct UpdateAcceptanceState { UpdateAcceptanceChannel channels[2]; };
enum class UpdateAcceptanceError : u8 {
    none, invalid_input, path, lock, missing, open, file, size, read,
    format, checksum, state, manifest, write, flush, commit, release,
};
struct UpdateAcceptanceResult {
    UpdateAcceptanceError error;
    DWORD native_error;
    UpdateManifestError manifest_error;
    constexpr bool ok() const noexcept { return error == UpdateAcceptanceError::none; }
};

// Caller-owned, zero-initialized, noncopyable by contract. The exclusive sibling
// .lock handle serializes cooperating processes for the entire verify/commit.
// Release explicitly. Do not modify fields, share a transaction between threads,
// or close/duplicate its handle. No heap, worker, installation or trust key is held.
struct UpdateAcceptanceTransaction {
    HANDLE lock;
    UpdateAcceptanceState state;
    wchar_t path[kUpdateAcceptanceMaximumPathCharacters + 1];
    u32 path_characters;
    bool record_present;
};

// Fixed little-endian record, zero reserved bytes and corruption checksum.
// The checksum is not authentication; the caller must protect storage with ACLs.
// Encode/decode and load leave output unchanged on every failure.
UpdateAcceptanceResult update_acceptance_encode(const UpdateAcceptanceState& state, Slice<u8> record) noexcept;
UpdateAcceptanceResult update_acceptance_decode(ConstSlice<u8> record, UpdateAcceptanceState* state) noexcept;
UpdateAcceptanceResult update_acceptance_load(ConstSlice<wchar_t> trusted_absolute_path, UpdateAcceptanceState* state) noexcept;

// Trusted LOCAL drive path only, count excludes NUL. Parent directories already
// exist and must be protected from hostile rename/reparse/write/delete. Rejects
// device/UNC/ADS/dot/trailing-dot paths, observed ancestor and final reparse points.
// These path checks do not secure writable ancestors against replacement races.
// Missing returns `missing` by default. Explicit first-use provisioning may pass
// allow_first_use=true to begin with record_present=false and empty state. Never
// infer first use from a missing record: enrolled storage erasure must fail closed.
// All corrupt/unreadable records fail closed. No automatic reset or repair.
// Output must own no handle. Siblings .lock and .new are reserved to this API.
UpdateAcceptanceResult update_acceptance_begin(ConstSlice<wchar_t> trusted_absolute_path,
    UpdateAcceptanceTransaction* transaction, bool allow_first_use = false) noexcept;
UpdateAcceptanceResult update_acceptance_capture_policy(const UpdateAcceptanceTransaction& transaction,
    const UpdateManifestPolicy& trusted_policy, UpdateManifestPolicy* captured_policy) noexcept;

// Signature verification and freshness/rollback check occur while holding the
// lock against the current disk snapshot. Trust key/origin never come from the
// offer. A caller's stronger rollback floor cannot be silently lowered.
// Writes a CREATE_NEW sibling, checks write/flush/close, then same-volume
// replacement with MOVEFILE_WRITE_THROUGH. Updates the manifest and policy rollback
// fields only after the commit succeeds. Retrying the same version with the same
// digest succeeds; all lower or changed same-version offers fail. The persisted
// identity is the signed-domain document digest used by update_manifest_verify.
// Only a successful call authorizes subsequent artifact verification/acceptance.
// All arguments and transaction must be disjoint; failures preserve manifest,
// policy and transaction. Caller must keep the key/freshness inputs trusted.
UpdateAcceptanceResult update_acceptance_verify_commit(UpdateAcceptanceTransaction* transaction,
    Utf8View document, ConstSlice<u8> signature, const P256Key& trusted_key,
    KdfSha256Workspace& workspace, UpdateManifestPolicy* policy,
    Slice<u32> structural_scratch, UpdateManifest* approved_manifest) noexcept;
UpdateAcceptanceResult update_acceptance_release(UpdateAcceptanceTransaction* transaction) noexcept;

} // namespace maicord
