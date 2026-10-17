#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/model/user_profile.hpp"
#include "maicord/model/user_activity.hpp"

namespace maicord {

inline constexpr u32 kUserDisplayNameMaximumBytes = 128;
inline constexpr u32 kUserAvatarHashMaximumBytes = 34;

struct UserRecord {
    u64 id;
    u32 name_offset;
    u32 last_seen;
    u32 revision;
    u16 name_bytes;
    u16 username_bytes;
    u16 generation;
    u8 avatar_hash_bytes;
    bool occupied;
    char avatar_hash[kUserAvatarHashMaximumBytes];
    bool pinned;
    // Presence client_status, two bits per platform (protocol/client_status.hpp);
    // zero until a presence reports a platform.
    u8 client_status;
};

struct UserStore {
    UserAppearanceStore appearance;
    // Rich presence per user (see user_activity.hpp); revision advances with
    // UserStore::revision when a visible activity changes.
    UserActivityStore activities;
    UserRecord* records;
    u32* order;
    u8* names;
    u32 capacity;
    u32 count;
    u32 name_bank_bytes;
    u32 name_used;
    u32 name_wasted;
    u32 clock;
    u32 revision;
    u32 mutations;
    u32 evictions;
    u32 compactions;
    u32 rejected;
    u8 active_name_bank;
    // Advances whenever every pin is cleared (fills padding).
    u16 pin_epoch;
};

enum class UserStoreError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    name_too_large,
    name_capacity,
};

struct UserStoreResult {
    UserStoreError error;
    bool inserted;
    bool name_changed;
    bool evicted;

    constexpr bool ok() const noexcept {
        return error == UserStoreError::none;
    }
};

bool user_store_initialize(UserStore* store,
                                   Slice<UserRecord> records,
                                   Slice<u32> order,
                                   Slice<u8> double_buffered_names) noexcept;
// For zero-filled (freshly committed) arrays: leaves their contents unchanged,
// so record pages are touched only as slots fill. The count bounds order reads.
bool user_store_initialize_sparse(UserStore* store, Slice<UserRecord> records, Slice<u32> order,
                                  Slice<u8> double_buffered_names) noexcept;
void user_store_clear_pins(UserStore* store) noexcept;
bool user_store_pin(UserStore* store, u64 id) noexcept;
UserStoreResult user_store_upsert(UserStore* store,
                                                 u64 id,
                                                 Utf8View display_name, Utf8View username = {}) noexcept;
bool user_store_set_avatar(UserStore* store, u64 id,
                                   Utf8View avatar_hash) noexcept;
// Per-platform presence for a resident user; true when it changed. It never
// advances the store revision: presence repaints are coalesced by the caller.
bool user_store_set_client_status(UserStore* store, u64 id, u8 client_status) noexcept;
// Zero (no platform online) for a user that is not resident.
u8 user_store_client_status(const UserStore& store, u64 id) noexcept;
const UserRecord* user_store_find(const UserStore& store,
                                                 u64 id) noexcept;
Utf8View user_store_name(const UserStore& store,
                                 const UserRecord& record) noexcept;
Utf8View user_store_username(const UserStore& store, const UserRecord& record) noexcept;
Utf8View user_store_avatar_hash(
    const UserRecord& record) noexcept;
u32 user_avatar_variant(Utf8View avatar_hash) noexcept;

} // namespace maicord
