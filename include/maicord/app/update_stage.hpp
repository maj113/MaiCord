#pragma once

#include "maicord/app/update_artifact.hpp"

namespace maicord {

inline constexpr u32 kUpdateStageFilenameCharacters = 128;
enum class UpdateStageError : u8 {
    none, invalid_input, identity, path, parent, source, size, create,
    read, write, hash, digest, flush, retain, release,
};
struct UpdateStageResult {
    UpdateStageError error;
    DWORD native_error;
    i32 crypto_status;
    DWORD cleanup_error;
    HANDLE cleanup_file;
    HANDLE cleanup_duplicate;
    HANDLE cleanup_parent;
    void* cleanup_hash;
    constexpr bool ok() const noexcept { return error == UpdateStageError::none && !cleanup_error; }
};

// Caller-owned and noncopyable by contract. file permits read and explicit
// disposal, but no writes; its sharing denies other writers/deletion.
// Release before destruction. No worker, installation or restart.
struct UpdateStage {
    HANDLE file;
    u64 bytes;
    u64 version;
    u8 sha256[32];
    u8 manifest_sha256[32];
    char artifact_name[kUpdateArtifactNameCapacity];
    u32 artifact_name_bytes;
};

// Call only after signature verification and durable rollback acceptance (the
// update_acceptance gate). Matching these structs never establishes authenticity.
// Parent and ancestors must be trusted, access-controlled, stable directories;
// the caller must derive the unique filename from the signed, accepted plan.
// Ordinary absolute drive/UNC parent paths with valid UTF-16 components only;
// filename uses ASCII letters/digits/dot/hyphen/underscore and ends .stage.
// CREATE_NEW refuses every existing target, including
// shipped executables/DLLs; no rename, overwrite, archive extraction or download.
// Inputs/output may not alias; output owns no handle. Serialize calls accessing
// the source cursor and release. The source remains owned and is read from zero.
// Copies and hashes the same fixed 4096-byte buffer; checks the u64 size,
// identity and digest, flushes, then returns a write-restricted handle.
// Failure leaves output unchanged; cleanup_error reports failed native cleanup.
// Any nonnull cleanup handles remain caller-owned and require cleanup_release;
// they are cleanup-only capabilities, never accepted stage candidates.
// cleanup_hash is a CNG handle, never a CloseHandle-compatible file handle.
UpdateStageResult update_stage_copy(UpdateArtifact* source,
    const UpdateManifest& durably_approved_manifest, ConstSlice<wchar_t> trusted_parent,
    ConstSlice<wchar_t> approved_stage_filename, UpdateStage* output) noexcept;

// discard=true deletes through the retained handle, never through a pathname.
// discard=false preserves the durable file; after release its bytes/identity are
// untrusted until reverified, and no serialized HANDLE is valid across processes.
// A failed disposition/close keeps the handle for a retry.
UpdateStageResult update_stage_release(UpdateStage* stage, bool discard) noexcept;
// Retry disposal of handles kept in a failed copy result. Leaves failed
// handles in the result, clears each successfully closed handle in place.
UpdateStageResult update_stage_cleanup_release(UpdateStageResult* failed_copy) noexcept;

} // namespace maicord
