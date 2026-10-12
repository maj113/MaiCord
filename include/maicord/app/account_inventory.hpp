#pragma once

#include "maicord/app/account_paths.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {

// Saved accounts for "Switch accounts". The inventory holds public identity
// only (user ID, names, avatar hash) and the slot whose DPAPI-protected
// profile carries the token; credentials never enter this file.
inline constexpr u32 kAccountInventoryVersion = 1;
inline constexpr u32 kAccountInventoryMaximumAccounts = 5;
inline constexpr u32 kAccountInventoryDisplayNameBytes = 128;
inline constexpr u32 kAccountInventoryUsernameBytes = 64;
inline constexpr u32 kAccountInventoryAvatarHashBytes = 40;
inline constexpr u32 kAccountInventoryMaximumFileBytes = 2048;

// The saved token was rejected; selecting the account asks to log in again.
inline constexpr u8 account_inventory_needs_login = 1u << 0;

struct AccountInventoryEntry {
    u64 user_id;
    u64 last_used_unix_ms;
    u8 slot;
    u8 flags;
    u8 display_name_bytes;
    u8 username_bytes;
    u8 avatar_hash_bytes;
    char display_name[kAccountInventoryDisplayNameBytes];
    char username[kAccountInventoryUsernameBytes];
    char avatar_hash[kAccountInventoryAvatarHashBytes];
};

struct AccountInventory {
    AccountInventoryEntry entries[kAccountInventoryMaximumAccounts];
    u32 count;
    u32 active_slot;
};
static_assert(sizeof(AccountInventory) <= 2048);

enum class AccountInventoryError : u8 {
    none,
    invalid_input,
    path,
    missing,
    open,
    size,
    read,
    format,
    version,
    write,
    capacity,
};

struct AccountInventoryResult {
    u32 native_error;
    AccountInventoryError error;
    bool migrated;

    constexpr bool ok() const noexcept {
        return error == AccountInventoryError::none;
    }
};

// <application_directory>\accounts\accounts.inventory for the slot-0 paths.
bool account_inventory_path(const AccountPaths& root, wchar_t* output, u32 capacity) noexcept;
AccountInventoryResult account_inventory_load(const wchar_t* path, AccountInventory* inventory) noexcept;
AccountInventoryResult account_inventory_save(const wchar_t* path, const AccountInventory& inventory) noexcept;
// Loads the inventory. Without one, the single-profile layout's saved account
// (slot 0) is adopted from its plaintext profile header (the credential is
// never read or decrypted) and the new inventory is saved. No profile and no
// inventory yields an empty inventory without writing anything.
AccountInventoryResult account_inventory_open(const AccountPaths& root, AccountInventory* inventory,
                                              u64 now_unix_ms) noexcept;
// Reads only the 48-byte header of a protected profile; its ID is the user ID.
bool account_inventory_profile_user(const wchar_t* profile_path, u64* user_id) noexcept;

const AccountInventoryEntry* account_inventory_find_slot(const AccountInventory& inventory, u32 slot) noexcept;
const AccountInventoryEntry* account_inventory_find_user(const AccountInventory& inventory,
                                                         u64 user_id) noexcept;
// Replaces the entry with the same slot or appends one (capacity error at the limit).
AccountInventoryError account_inventory_upsert(AccountInventory* inventory,
                                               const AccountInventoryEntry& entry) noexcept;
bool account_inventory_remove_slot(AccountInventory* inventory, u32 slot) noexcept;
// Marks the slot active and most recently used.
bool account_inventory_touch(AccountInventory* inventory, u32 slot, u64 now_unix_ms) noexcept;
// Lowest unused slot; false when the account limit is reached.
bool account_inventory_free_slot(const AccountInventory& inventory, u32* slot) noexcept;
// Entry indices, most recently used first (ties: lower slot first).
u32 account_inventory_order(const AccountInventory& inventory, u8* indices, u32 capacity) noexcept;
// Copies display identity (whole code points, bounded); true when it changed.
bool account_inventory_set_identity(AccountInventoryEntry* entry, Utf8View display_name, Utf8View username,
                                    Utf8View avatar_hash) noexcept;
inline Utf8View account_inventory_display_name(const AccountInventoryEntry& entry) noexcept {
    return {entry.display_name, entry.display_name_bytes};
}
inline Utf8View account_inventory_username(const AccountInventoryEntry& entry) noexcept {
    return {entry.username, entry.username_bytes};
}
inline Utf8View account_inventory_avatar_hash(const AccountInventoryEntry& entry) noexcept {
    return {entry.avatar_hash, entry.avatar_hash_bytes};
}

} // namespace maicord
