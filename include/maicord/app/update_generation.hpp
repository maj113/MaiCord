#pragma once

#include "maicord/app/update_pair_package.hpp"

namespace maicord {

// Backend journal v2. V1/unknown records are refused; there is no implicit
// migration or missing-journal reset. Migrating an enrolled v1 installation
// needs a separately authenticated policy.
inline constexpr u32 kUpdateGenerationRecordVersion = 2;
inline constexpr u32 kUpdateGenerationRecordBytes = 768;
inline constexpr u32 kUpdateGenerationMaximumRootCharacters = 511;
inline constexpr u32 kUpdateGenerationMaximumAncestors = 32;
inline constexpr u32 kUpdateGenerationMaximumRootEntries = 16;

struct UpdateGenerationIdentity { u8 bytes[16]; };
struct UpdateGenerationReference { UpdatePairDescriptor pair; bool present; };
struct UpdateGenerationDirectoryIdentity { DWORD volume; u64 file; };
enum class UpdateGenerationDisposalTarget : u8 { none, previous, candidate };
struct UpdateGenerationDisposalIntent {
    UpdateGenerationDisposalTarget target;
    UpdateGenerationDirectoryIdentity directory;
    UpdateGenerationDirectoryIdentity files[5]; // fixed EXE, DLL, offer, signature, package
};
enum class UpdateGenerationPhase : u8 { stable, prepared, pending, acknowledged, rollback };
struct UpdateGenerationState {
    UpdateGenerationIdentity installation;
    UpdateGenerationIdentity transaction;
    u64 sequence;
    UpdateGenerationPhase phase;
    UpdateGenerationReference active, previous, candidate;
    UpdateGenerationDisposalIntent disposal;
};
enum class UpdateGenerationError : u8 {
    none, invalid_input, format, checksum, state, identity, path, enrollment,
    gate, open, file, size, read, write, flush, commit, release, authenticity, cleanup,
};
struct UpdateGenerationResult {
    UpdateGenerationError error;
    DWORD native_error;
    constexpr bool ok() const noexcept { return error == UpdateGenerationError::none; }
};
enum class UpdateGenerationRecovery : u8 { launch_active, retain_prepared, roll_back, finish_acknowledgement, discard_candidate, discard_previous };

// Codec and transitions are pure plans, not installation/publisher authorization.
// All outputs remain unchanged on failure. Descriptor checksums detect damage;
// only verify_installed with the enrolled publisher inventory establishes trust.
UpdateGenerationResult update_generation_encode(const UpdateGenerationState&, Slice<u8>) noexcept;
UpdateGenerationResult update_generation_decode(ConstSlice<u8>, UpdateGenerationState*) noexcept;
bool update_generation_pair_equal(const UpdatePairDescriptor&, const UpdatePairDescriptor&) noexcept;
UpdateGenerationResult update_generation_prepare(const UpdateGenerationState&, const UpdateGenerationIdentity& transaction,
    const UpdatePairDescriptor& candidate, UpdateGenerationState*) noexcept;
UpdateGenerationResult update_generation_activate(const UpdateGenerationState&, UpdateGenerationState*) noexcept;
UpdateGenerationResult update_generation_acknowledge(const UpdateGenerationState&, const UpdateGenerationIdentity& transaction,
    ConstSlice<u8> generation_document_sha256, UpdateGenerationState*) noexcept;
UpdateGenerationResult update_generation_rollback(const UpdateGenerationState&, UpdateGenerationState*) noexcept;
// Cleanup completion is supplied only after the generation directory is empty
// and its files were deleted. No unknown entry or recursive deletion.
UpdateGenerationResult update_generation_finish(const UpdateGenerationState&, bool candidate_disposed, UpdateGenerationState*) noexcept;
UpdateGenerationResult update_generation_forget_previous(const UpdateGenerationState&, bool previous_disposed, UpdateGenerationState*) noexcept;
// User-requested return to the retained previous pair: stable, no disposal
// intent; swaps active and previous. Both stay authenticated launch targets.
UpdateGenerationResult update_generation_revert(const UpdateGenerationState&, UpdateGenerationState*) noexcept;
// Plan only. Committing reauthenticates the complete target and reopens all
// fixed identities under the gate before granting permission for
// absence-tolerant cold disposal. The intent is durable before the first delete.
UpdateGenerationResult update_generation_disposal_begin(const UpdateGenerationState&,
    const UpdateGenerationDisposalIntent&, UpdateGenerationState*) noexcept;
UpdateGenerationResult update_generation_recovery(const UpdateGenerationState&, UpdateGenerationRecovery*) noexcept;

// Provisioning defines enrollment and ACL policy. Every canonical drive/ancestor
// directory identity, including the drive root and final root, is passed in.
// Missing enrollment or journal is never silently treated as first use. The
// publisher inventory fingerprint is supplied by enrollment, never an offer.
struct UpdateGenerationEnrollment {
    ConstSlice<wchar_t> root;
    UpdateGenerationIdentity installation;
    ConstSlice<UpdateGenerationDirectoryIdentity> ancestors;
    u8 publisher_inventory_sha256[32];
    bool explicitly_enrolled;
};
struct UpdateGenerationTransaction {
    HANDLE gate;
    HANDLE ancestors[kUpdateGenerationMaximumAncestors];
    UpdateGenerationDirectoryIdentity identities[kUpdateGenerationMaximumAncestors];
    u32 ancestor_count;
    wchar_t root[kUpdateGenerationMaximumRootCharacters + 1];
    u32 root_characters;
    UpdateGenerationIdentity installation;
    u8 publisher_inventory_sha256[32];
    UpdateGenerationState state;
    bool record_present;
    bool provisioning_admitted; // set only by a successful missing-journal begin
    HANDLE record_owner; // failed write/flush/close/disposal remains retryable
    bool record_temporary;
    HANDLE validation_owner; // failed transient validation close, never disposable
    HANDLE inventory_owner; // closed with FindClose, never CloseHandle
};
// Callback must reopen fixed provenance/package/pair paths beneath the retained
// root, reverify the retained signed offer under this inventory, rehash package,
// derive descriptor via update_pair_package_verify_installed and compare every
// field. It must never accept a caller descriptor checksum as proof of the publisher.
struct UpdateGenerationAuthenticator {
    void* context;
    bool (*verify_installed)(void*, UpdateGenerationTransaction*, const UpdatePairDescriptor&, DWORD*) noexcept;
};

// Caller-owned, zero initialized and noncopyable by contract. Holds directory
// ancestry without delete sharing and one exclusive installation gate. Missing
// journal is accepted only for explicit provisioning, never ordinary recovery.
// On every failure, returned handles in the transaction stay there for release.
UpdateGenerationResult update_generation_begin(const UpdateGenerationEnrollment&, UpdateGenerationTransaction*,
    bool explicit_provision = false) noexcept;
UpdateGenerationResult update_generation_validate(UpdateGenerationTransaction*) noexcept;
// Enforces the legal one-step journal transition and authenticates launchable
// generations under the gate before commit, including prepared candidates.
// Recording disposal intent also authenticates its complete target and compares
// directory/five file identities. Finalization requires that captured
// directory be absent; partial cold cleanup is authorized only by protected local
// intent, never by checksum or caller completion booleans alone.
// A failed rollback candidate cannot veto authentication of the old active pair.
// Flush + close then same-volume replacement of one record selects an entire
// pair. No acceptance write.
UpdateGenerationResult update_generation_commit(UpdateGenerationTransaction*, const UpdateGenerationState&,
    const UpdateGenerationAuthenticator&) noexcept;
// Authenticate before launch selection. Pending never manufactures success:
// cold recovery must commit rollback, or acknowledgement completion, first.
UpdateGenerationResult update_generation_select(UpdateGenerationTransaction*, const UpdateGenerationAuthenticator&,
    UpdatePairDescriptor*) noexcept;
UpdateGenerationResult update_generation_release(UpdateGenerationTransaction*) noexcept;
UpdateGenerationResult update_generation_cleanup_record(UpdateGenerationTransaction*) noexcept;
// A crash-left .new is never committed implicitly. Only a valid, authenticated
// legal successor to the loaded journal can be adopted for disposal by handle.
// Torn, alien or unverifiable metadata fails closed and is never deleted.
UpdateGenerationResult update_generation_recover_temporary(UpdateGenerationTransaction*, const UpdateGenerationAuthenticator&) noexcept;

enum class UpdateGenerationFile : u8 { executable, account, offer, signature, package };
struct UpdateGenerationDirectory {
    UpdateGenerationTransaction* transaction;
    HANDLE directory;
    UpdateGenerationDirectoryIdentity identity;
    u8 document_sha256[32];
    bool created;
    bool disposal_authorized;
    bool identity_captured;
};
// Transaction outlives all borrowed directories; close files and directories
// before releasing its gate. All names derive from the signed digest:
// g-<64 lowercase hex>. File
// basenames are fixed. No command-line/offer path is accepted. CREATE_NEW only.
UpdateGenerationResult update_generation_directory_open(UpdateGenerationTransaction*, ConstSlice<u8> document_sha256,
    bool create_new, UpdateGenerationDirectory*) noexcept;
UpdateGenerationResult update_generation_directory_validate(UpdateGenerationDirectory*) noexcept;
// Reopened previous/candidate directories require durable captured intent and
// fresh active-pair authenticity before their files/directory are discarded.
UpdateGenerationResult update_generation_directory_authorize_disposal(UpdateGenerationDirectory*,
    const UpdateGenerationAuthenticator&) noexcept;
// Cold disposal uses the durable intent, independently authenticates the
// active pair, and never reauthenticates missing target provenance. Existing
// directory identity must match; absent means the captured directory was already
// disposed. Unknown/reparse/replaced content is never adopted or removed.
UpdateGenerationResult update_generation_disposal_directory_open(UpdateGenerationTransaction*,
    UpdateGenerationDirectory*, bool* already_absent) noexcept;
UpdateGenerationResult update_generation_disposal_inventory(UpdateGenerationDirectory*) noexcept;
UpdateGenerationResult update_generation_file_open(UpdateGenerationDirectory*, UpdateGenerationFile, bool create_new, HANDLE*) noexcept;
UpdateGenerationResult update_generation_file_seal(UpdateGenerationDirectory*, HANDLE* writable, HANDLE* readonly) noexcept;
UpdatePairTarget update_generation_pair_target(UpdateGenerationDirectory*) noexcept;
UpdateGenerationResult update_generation_path(const UpdateGenerationTransaction&, ConstSlice<u8> document_sha256,
    UpdateGenerationFile, Slice<wchar_t>, u32* characters) noexcept;
// discard requires this process's created directory and fails on unknown entries.
// The caller first disposes only its own file handles. Failure keeps the handles.
UpdateGenerationResult update_generation_directory_release(UpdateGenerationDirectory*, bool discard) noexcept;

} // namespace maicord
