#pragma once

#include "maicord/app/account_profile.hpp"
#include "maicord/model/message_draft_store.hpp"

namespace maicord {

inline constexpr u32 kMessageDraftPersistenceVersion = 2;
inline constexpr u32 kMessageDraftPersistenceMaximumFileBytes =
    128 * 1024;

enum class MessageDraftPersistenceError : u8 {
    none,
    invalid_input,
    memory,
    open,
    size,
    read,
    format,
    version,
    profile,
    capacity,
    write,
    flush,
    commit,
};

struct MessageDraftPersistenceResult {
    u32 bytes;
    u32 records;
    u32 discarded;
    u32 native_error;
    MessageDraftPersistenceError error;

    constexpr bool ok() const noexcept {
        return error == MessageDraftPersistenceError::none;
    }
};

// The file is profile-bound, versioned, checksummed, and committed through a
// write-through sibling before replacement. Load validates the entire file
// before modifying the caller-owned store. Only the current version loads;
// any other version fails with `version` and is treated as an absent store.
MessageDraftPersistenceResult message_drafts_save_file(
    const wchar_t* path, u64 profile_id,
    const MessageDraftStore& store) noexcept;
// Split save for off-thread commits: encode snapshots the validated store
// into an owned image; commit consumes (and always releases) it. Background
// draft commits skip write-through; the replace stays atomic and load still
// rejects any torn or unflushed image by checksum.
struct MessageDraftImage {
    u8* bytes;
    u32 count;
    u32 records;
};
MessageDraftPersistenceResult message_drafts_encode(u64 profile_id, const MessageDraftStore& store,
                                                    MessageDraftImage* image) noexcept;
MessageDraftPersistenceResult message_drafts_commit(const wchar_t* path, MessageDraftImage* image,
                                                    bool write_through) noexcept;
void message_drafts_image_release(MessageDraftImage* image) noexcept;
MessageDraftPersistenceResult message_drafts_load_file(
    const wchar_t* path, u64 expected_profile_id,
    MessageDraftStore* store) noexcept;

} // namespace maicord
