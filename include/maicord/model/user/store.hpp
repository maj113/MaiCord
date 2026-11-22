#pragma once

#include "maicord/model/user/profile.hpp"
#include "maicord/model/user/activity.hpp"

namespace maicord {

inline constexpr u32 kUserDisplayNameMaximumBytes = 128;
inline constexpr u32 kUserAvatarHashMaximumBytes = 34;

struct UserRecord {
    u64 id;
    u32 name_offset;
    u32 last_seen;
    u32 revision;
    u8 name_bytes;
    u8 username_bytes;
    u16 generation;
    u8 avatar_hash_bytes;
    bool occupied;
    // The username is the display name (a user without a global name): the bank holds the bytes once.
    bool username_shared;
    char avatar_hash[kUserAvatarHashMaximumBytes];
    // The pin mark of the store's pin epoch when the record was pinned; read through user_store_pinned.
    u8 pin_mark;
    // Presence client_status, two bits per platform (protocol/client_status.hpp);
    // zero until a presence reports a platform.
    u8 client_status;
};

struct UserStore {
    UserAppearanceStore appearance;
    // Rich presence per user (see activity.hpp); revision advances with
    // UserStore::revision when a visible activity changes.
    UserActivityStore activities;
    UserRecord* records;
    u32* order;
    u8* names;
    // The address range of user_store_initialize_owned (records, order, then the two name banks), committed as the
    // store fills. Null for caller storage, which is writable whole.
    u8* reservation;
    u32 capacity;
    // The records and order entries that are writable.
    u32 committed;
    u32 count;
    u32 name_bank_bytes;
    // The writable bytes at the start of each name bank.
    u32 name_committed[2];
    u32 name_used;
    u32 name_wasted;
    u32 clock;
    u32 revision;
    u32 mutations;
    u32 evictions;
    u32 compactions;
    u32 rejected;
    // Every slot below free_hint is occupied, so the search for a free slot starts there. Every occupied slot
    // lies below slot_extent, so scans of all records stop there.
    u32 free_hint;
    u32 slot_extent;
    u8 active_name_bank;
    // The mark that pins a record now, 1 to 255; initialization sets 1, so a reset record (mark 0) is never
    // pinned. Each pin clear moves to the next mark, so earlier marks no longer match. When the marks start
    // again at 1 (each 255 clears), the clear sets every mark to 0 first.
    u8 pin_mark;
    // Advances whenever every pin is cleared.
    u16 pin_epoch;
};

inline bool user_store_pinned(const UserStore& store, const UserRecord& record) noexcept {
    return record.pin_mark == store.pin_mark;
}

enum class UserStoreError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    name_too_large,
    name_capacity,
    // The system refused to commit owned storage.
    memory,
};

struct UserStoreResult {
    UserStoreError error;
    bool inserted;
    bool evicted;

    constexpr bool ok() const noexcept {
        return error == UserStoreError::none;
    }
};

// Reserves address space for `capacity` users and two name banks of `name_bank_bytes` (rounded up to whole pages), and
// commits it as users arrive: records in steps of kUserStoreCommitUsers (the order in steps of 4,096 users), names in
// steps of kUserStoreCommitNameBytes. A compaction returns the pages of the bank it leaves. The store evicts only when it
// holds `capacity` users or a full bank of live names. user_store_release returns the range.
inline constexpr u32 kUserStoreCommitUsers = 256;
inline constexpr u32 kUserStoreCommitNameBytes = 16 * 1024;
bool user_store_initialize_owned(UserStore* store, u32 capacity, u32 name_bank_bytes) noexcept;
// Releases owned storage and the appearance and activity stores; the store is empty afterwards.
void user_store_release(UserStore* store) noexcept;
void user_store_clear_pins(UserStore* store) noexcept;
bool user_store_pin(UserStore* store, u64 id) noexcept;
UserStoreResult user_store_upsert(UserStore* store,
                                                 u64 id,
                                                 const Utf8View& display_name, const Utf8View& username = {}) noexcept;
bool user_store_set_avatar(UserStore* store, u64 id,
                                   const Utf8View& avatar_hash) noexcept;
// Per-platform presence for a resident user; true when it changed. It never
// advances the store revision: presence repaints are coalesced by the caller.
bool user_store_set_client_status(UserStore* store, u64 id, u8 client_status) noexcept;
// Zero (no platform online) for a user that is not resident.
u8 user_store_client_status(const UserStore& store, u64 id) noexcept;
// A fresh Gateway session re-sends only non-offline presences: every user
// starts offline again. Returns whether any record changed.
bool user_store_clear_client_status(UserStore* store) noexcept;
const UserRecord* user_store_find(const UserStore& store,
                                                 u64 id) noexcept;
Utf8View user_store_name(const UserStore& store,
                                 const UserRecord& record) noexcept;
Utf8View user_store_username(const UserStore& store, const UserRecord& record) noexcept;
Utf8View user_store_avatar_hash(
    const UserRecord& record) noexcept;
u32 user_avatar_variant(const Utf8View& avatar_hash) noexcept;

} // namespace maicord
