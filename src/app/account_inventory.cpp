#include "maicord/core/win32_process.hpp"
#include "maicord/app/account_inventory.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u8 kMagic[8] = {'M', 'A', 'I', 'C', 'A', 'C', 'C', 'T'};
inline constexpr u8 kProfileMagic[8] = {'M', 'A', 'I', 'C', 'U', 'S', 'E', 'R'};
inline constexpr u32 kHeaderBytes = 32;
inline constexpr u32 kEntryBytes = 24 + kAccountInventoryDisplayNameBytes + kAccountInventoryUsernameBytes +
                                   kAccountInventoryAvatarHashBytes;
static_assert(kEntryBytes == 256);
static_assert(kHeaderBytes + kEntryBytes * kAccountInventoryMaximumAccounts <= kAccountInventoryMaximumFileBytes);
inline constexpr u32 kProfileHeaderBytes = 48;

AccountInventoryResult failure(AccountInventoryError error, u32 native_error = 0) noexcept {
    return AccountInventoryResult{native_error, error, false};
}


bool label_valid(const char* bytes, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index)
        if (static_cast<u8>(bytes[index]) < 0x20 || bytes[index] == 0x7f)
            return false;
    // A stored label never ends inside a code point.
    if (count && (static_cast<u8>(bytes[count - 1]) & 0x80)) {
        u32 lead = count - 1;
        while (lead && (static_cast<u8>(bytes[lead]) & 0xc0) == 0x80)
            --lead;
        const u8 first = static_cast<u8>(bytes[lead]);
        const u32 width = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : first >= 0xc0 ? 2 : 0;
        if (width == 0 || count - lead != width)
            return false;
    }
    return true;
}

bool entry_valid(const AccountInventoryEntry& entry) noexcept {
    return entry.user_id != 0 && entry.slot < kAccountPathsSlotCount &&
           (entry.flags & ~account_inventory_needs_login) == 0 &&
           entry.display_name_bytes <= kAccountInventoryDisplayNameBytes &&
           entry.username_bytes <= kAccountInventoryUsernameBytes &&
           entry.avatar_hash_bytes <= kAccountInventoryAvatarHashBytes &&
           label_valid(entry.display_name, entry.display_name_bytes) &&
           label_valid(entry.username, entry.username_bytes) &&
           label_valid(entry.avatar_hash, entry.avatar_hash_bytes);
}

bool inventory_valid(const AccountInventory& inventory) noexcept {
    if (inventory.count > kAccountInventoryMaximumAccounts || inventory.active_slot >= kAccountPathsSlotCount)
        return false;
    for (u32 index = 0; index < inventory.count; ++index) {
        const auto& entry = inventory.entries[index];
        if (!entry_valid(entry))
            return false;
        for (u32 other = 0; other < index; ++other)
            if (inventory.entries[other].slot == entry.slot || inventory.entries[other].user_id == entry.user_id)
                return false;
    }
    return true;
}

void encode_entry(u8* output, const AccountInventoryEntry& entry) noexcept {
    clear_bytes(output, kEntryBytes);
    byte_store_le64(output, entry.user_id);
    byte_store_le64(output + 8, entry.last_used_unix_ms);
    output[16] = entry.slot;
    output[17] = entry.flags;
    output[18] = entry.display_name_bytes;
    output[19] = entry.username_bytes;
    output[20] = entry.avatar_hash_bytes;
    u32 offset = 24;
    copy_bytes(output + offset, entry.display_name, entry.display_name_bytes);
    offset += kAccountInventoryDisplayNameBytes;
    copy_bytes(output + offset, entry.username, entry.username_bytes);
    offset += kAccountInventoryUsernameBytes;
    copy_bytes(output + offset, entry.avatar_hash, entry.avatar_hash_bytes);
}

bool decode_entry(const u8* input, AccountInventoryEntry* entry) noexcept {
    AccountInventoryEntry decoded{};
    decoded.user_id = byte_load_le64(input);
    decoded.last_used_unix_ms = byte_load_le64(input + 8);
    decoded.slot = input[16];
    decoded.flags = input[17];
    decoded.display_name_bytes = input[18];
    decoded.username_bytes = input[19];
    decoded.avatar_hash_bytes = input[20];
    if (input[21] || input[22] || input[23] || decoded.display_name_bytes > kAccountInventoryDisplayNameBytes ||
        decoded.username_bytes > kAccountInventoryUsernameBytes ||
        decoded.avatar_hash_bytes > kAccountInventoryAvatarHashBytes)
        return false;
    u32 offset = 24;
    copy_bytes(decoded.display_name, input + offset, decoded.display_name_bytes);
    offset += kAccountInventoryDisplayNameBytes;
    copy_bytes(decoded.username, input + offset, decoded.username_bytes);
    offset += kAccountInventoryUsernameBytes;
    copy_bytes(decoded.avatar_hash, input + offset, decoded.avatar_hash_bytes);
    if (!entry_valid(decoded))
        return false;
    *entry = decoded;
    return true;
}

// Copies a bounded label prefix without splitting a code point.
u8 copy_label(char* output, u32 capacity, Utf8View value) noexcept {
    if (!value.data || !value.bytes)
        return 0;
    u32 bytes = minimum(value.bytes, capacity);
    while (bytes < value.bytes && bytes && (static_cast<u8>(value.data[bytes]) & 0xc0) == 0x80)
        --bytes;
    u32 kept = 0;
    for (; kept < bytes; ++kept)
        if (static_cast<u8>(value.data[kept]) < 0x20 || value.data[kept] == 0x7f)
            break;
    // A control byte ends the label; trim back to a code point boundary.
    while (kept < bytes && kept && (static_cast<u8>(value.data[kept]) & 0xc0) == 0x80)
        --kept;
    copy_bytes(output, value.data, kept);
    return static_cast<u8>(kept);
}

} // namespace

bool account_inventory_path(const AccountPaths& root, wchar_t* output, u32 capacity) noexcept {
    u32 length = 0;
    return output && capacity && root.application_directory[0] &&
           wide_append(output, capacity, &length, root.application_directory) &&
           wide_append(output, capacity, &length, L"\\accounts\\accounts.inventory");
}

AccountInventoryResult account_inventory_load(const wchar_t* path, AccountInventory* inventory) noexcept {
    const u32 characters = wide_length(path, kAccountProfileMaximumPathCharacters);
    if (!characters || characters >= kAccountProfileMaximumPathCharacters || !inventory)
        return failure(AccountInventoryError::invalid_input, ERROR_INVALID_PARAMETER);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const u32 error = thread_last_error();
        return failure(win32_error_is_missing_file(error) ? AccountInventoryError::missing
                                                          : AccountInventoryError::open,
                       error);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < kHeaderBytes ||
        size.QuadPart > kAccountInventoryMaximumFileBytes) {
        const u32 error = thread_last_error();
        CloseHandle(file);
        return failure(AccountInventoryError::size, error);
    }
    u8 bytes[kAccountInventoryMaximumFileBytes];
    const u32 file_size = static_cast<u32>(size.QuadPart);
    u32 native_error = 0;
    const bool read = file_persistence_read_complete(file, Slice<u8>{bytes, file_size}, &native_error);
    CloseHandle(file);
    if (!read)
        return failure(AccountInventoryError::read, native_error);
    if (!bytes_equal(bytes, kMagic, sizeof(kMagic)))
        return failure(AccountInventoryError::format);
    if (byte_load_le32(bytes + 8) != kAccountInventoryVersion)
        return failure(AccountInventoryError::version);
    const u32 count = byte_load_le32(bytes + 20);
    const u32 active_slot = byte_load_le32(bytes + 24);
    if (byte_load_le32(bytes + 12) != kHeaderBytes || byte_load_le32(bytes + 16) != file_size ||
        byte_load_le32(bytes + 28) != 0 || count > kAccountInventoryMaximumAccounts ||
        file_size != kHeaderBytes + count * kEntryBytes)
        return failure(AccountInventoryError::format);
    AccountInventory loaded{};
    loaded.count = count;
    loaded.active_slot = active_slot;
    for (u32 index = 0; index < count; ++index)
        if (!decode_entry(bytes + kHeaderBytes + index * kEntryBytes, &loaded.entries[index]))
            return failure(AccountInventoryError::format);
    if (!inventory_valid(loaded))
        return failure(AccountInventoryError::format);
    *inventory = loaded;
    return AccountInventoryResult{};
}

AccountInventoryResult account_inventory_save(const wchar_t* path, const AccountInventory& inventory) noexcept {
    const u32 characters = wide_length(path, kAccountProfileMaximumPathCharacters);
    if (!characters || characters + 4 >= kAccountProfileMaximumPathCharacters || !inventory_valid(inventory))
        return failure(AccountInventoryError::invalid_input, ERROR_INVALID_PARAMETER);
    u8 bytes[kAccountInventoryMaximumFileBytes]{};
    const u32 file_size = kHeaderBytes + inventory.count * kEntryBytes;
    copy_bytes(bytes, kMagic, sizeof(kMagic));
    byte_store_le32(bytes + 8, kAccountInventoryVersion);
    byte_store_le32(bytes + 12, kHeaderBytes);
    byte_store_le32(bytes + 16, file_size);
    byte_store_le32(bytes + 20, inventory.count);
    byte_store_le32(bytes + 24, inventory.active_slot);
    for (u32 index = 0; index < inventory.count; ++index)
        encode_entry(bytes + kHeaderBytes + index * kEntryBytes, inventory.entries[index]);
    const FilePersistenceWriteResult written =
        file_persistence_write_atomic(path, characters, ConstSlice<u8>{bytes, file_size});
    return written.ok() ? AccountInventoryResult{} : failure(AccountInventoryError::write, written.native_error);
}

bool account_inventory_profile_user(const wchar_t* profile_path, u64* user_id) noexcept {
    if (!user_id || !wide_length(profile_path, kAccountProfileMaximumPathCharacters))
        return false;
    HANDLE file = CreateFileW(profile_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    u8 header[kProfileHeaderBytes]{};
    u32 native_error = 0;
    const bool read = file_persistence_read_complete(file, Slice<u8>{header, kProfileHeaderBytes}, &native_error);
    CloseHandle(file);
    if (!read || !bytes_equal(header, kProfileMagic, sizeof(kProfileMagic)) ||
        byte_load_le32(header + 12) != kProfileHeaderBytes)
        return false;
    const u64 id = byte_load_le64(header + 32);
    if (!id)
        return false;
    *user_id = id;
    return true;
}

AccountInventoryResult account_inventory_open(const AccountPaths& root, AccountInventory* inventory,
                                              u64 now_unix_ms) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters];
    if (!inventory || !account_inventory_path(root, path, kAccountProfileMaximumPathCharacters))
        return failure(AccountInventoryError::path);
    const AccountInventoryResult loaded = account_inventory_load(path, inventory);
    if (loaded.error != AccountInventoryError::missing)
        return loaded;
    *inventory = {};
    u64 user_id = 0;
    if (!account_inventory_profile_user(root.profile, &user_id))
        return AccountInventoryResult{};
    // The single-profile layout becomes slot 0 in place; nothing is moved.
    AccountInventoryEntry entry{};
    entry.user_id = user_id;
    entry.slot = 0;
    entry.last_used_unix_ms = now_unix_ms;
    inventory->entries[0] = entry;
    inventory->count = 1;
    inventory->active_slot = 0;
    AccountInventoryResult saved = account_inventory_save(path, *inventory);
    saved.migrated = true;
    return saved;
}

const AccountInventoryEntry* account_inventory_find_slot(const AccountInventory& inventory, u32 slot) noexcept {
    for (u32 index = 0; index < inventory.count && index < kAccountInventoryMaximumAccounts; ++index)
        if (inventory.entries[index].slot == slot)
            return &inventory.entries[index];
    return nullptr;
}

const AccountInventoryEntry* account_inventory_find_user(const AccountInventory& inventory,
                                                         u64 user_id) noexcept {
    if (!user_id)
        return nullptr;
    for (u32 index = 0; index < inventory.count && index < kAccountInventoryMaximumAccounts; ++index)
        if (inventory.entries[index].user_id == user_id)
            return &inventory.entries[index];
    return nullptr;
}

AccountInventoryError account_inventory_upsert(AccountInventory* inventory,
                                               const AccountInventoryEntry& entry) noexcept {
    if (!inventory || !entry_valid(entry) || inventory->count > kAccountInventoryMaximumAccounts)
        return AccountInventoryError::invalid_input;
    const auto* same_user = account_inventory_find_user(*inventory, entry.user_id);
    if (same_user && same_user->slot != entry.slot)
        return AccountInventoryError::invalid_input;
    for (u32 index = 0; index < inventory->count; ++index)
        if (inventory->entries[index].slot == entry.slot) {
            inventory->entries[index] = entry;
            return AccountInventoryError::none;
        }
    if (inventory->count >= kAccountInventoryMaximumAccounts)
        return AccountInventoryError::capacity;
    inventory->entries[inventory->count++] = entry;
    return AccountInventoryError::none;
}

bool account_inventory_remove_slot(AccountInventory* inventory, u32 slot) noexcept {
    if (!inventory)
        return false;
    for (u32 index = 0; index < inventory->count; ++index)
        if (inventory->entries[index].slot == slot) {
            for (u32 next = index + 1; next < inventory->count; ++next)
                inventory->entries[next - 1] = inventory->entries[next];
            inventory->entries[--inventory->count] = {};
            return true;
        }
    return false;
}

bool account_inventory_touch(AccountInventory* inventory, u32 slot, u64 now_unix_ms) noexcept {
    if (!inventory)
        return false;
    for (u32 index = 0; index < inventory->count; ++index)
        if (inventory->entries[index].slot == slot) {
            // Keep a strict order even when two switches share a millisecond.
            u64 newest = now_unix_ms;
            for (u32 other = 0; other < inventory->count; ++other)
                if (other != index && inventory->entries[other].last_used_unix_ms >= newest)
                    newest = inventory->entries[other].last_used_unix_ms + 1;
            inventory->entries[index].last_used_unix_ms = newest;
            inventory->active_slot = slot;
            return true;
        }
    return false;
}

bool account_inventory_free_slot(const AccountInventory& inventory, u32* slot) noexcept {
    if (!slot || inventory.count >= kAccountInventoryMaximumAccounts)
        return false;
    for (u32 candidate = 0; candidate < kAccountPathsSlotCount; ++candidate)
        if (!account_inventory_find_slot(inventory, candidate)) {
            *slot = candidate;
            return true;
        }
    return false;
}

u32 account_inventory_order(const AccountInventory& inventory, u8* indices, u32 capacity) noexcept {
    const u32 count = minimum(minimum(inventory.count, kAccountInventoryMaximumAccounts), capacity);
    if (!indices)
        return 0;
    for (u32 index = 0; index < count; ++index)
        indices[index] = static_cast<u8>(index);
    for (u32 index = 1; index < count; ++index) {
        const u8 value = indices[index];
        const auto& entry = inventory.entries[value];
        u32 position = index;
        while (position) {
            const auto& previous = inventory.entries[indices[position - 1]];
            const bool before = entry.last_used_unix_ms > previous.last_used_unix_ms ||
                                (entry.last_used_unix_ms == previous.last_used_unix_ms && entry.slot < previous.slot);
            if (!before)
                break;
            indices[position] = indices[position - 1];
            --position;
        }
        indices[position] = value;
    }
    return count;
}

bool account_inventory_set_identity(AccountInventoryEntry* entry, Utf8View display_name, Utf8View username,
                                    Utf8View avatar_hash) noexcept {
    if (!entry)
        return false;
    AccountInventoryEntry updated = *entry;
    clear_bytes(updated.display_name, sizeof(updated.display_name));
    clear_bytes(updated.username, sizeof(updated.username));
    clear_bytes(updated.avatar_hash, sizeof(updated.avatar_hash));
    updated.display_name_bytes = copy_label(updated.display_name, kAccountInventoryDisplayNameBytes, display_name);
    updated.username_bytes = copy_label(updated.username, kAccountInventoryUsernameBytes, username);
    updated.avatar_hash_bytes = copy_label(updated.avatar_hash, kAccountInventoryAvatarHashBytes, avatar_hash);
    if (bytes_equal(&updated, entry, sizeof(updated)))
        return false;
    *entry = updated;
    return true;
}

} // namespace maicord
