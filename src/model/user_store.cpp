#include "maicord/model/user_store.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/asset_hash.hpp"

namespace maicord {
namespace {

constexpr u16 next_generation(u16 generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

u32 find_order(const UserStore& store, u64 id, bool* found) noexcept {
    u32 first = 0;
    u32 count = store.count;
    while (count != 0) {
        const u32 step = count / 2;
        const u32 current = first + step;
        const u64 current_id = store.records[store.order[current]].id;
        if (current_id < id) {
            first = current + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    *found = first < store.count && store.records[store.order[first]].id == id;
    return first;
}

u32 active_name_base(const UserStore& store) noexcept {
    return static_cast<u32>(store.active_name_bank) * store.name_bank_bytes;
}

u32 name_size(const UserRecord& record) noexcept { return record.name_bytes + record.username_bytes; }
bool compact_names(UserStore* store, u32 skipped_slot) noexcept {
    const u8 target_bank = store->active_name_bank == 0 ? 1 : 0;
    const u32 target_base = static_cast<u32>(target_bank) * store->name_bank_bytes;
    u32 used = 0;
    for (u32 order = 0; order < store->count; ++order) {
        const u32 slot = store->order[order];
        UserRecord& record = store->records[slot];
        if (slot == skipped_slot) {
            continue;
        }
        if (name_size(record) > store->name_bank_bytes - used) {
            return false;
        }
        copy_bytes(store->names + target_base + used, store->names + record.name_offset, name_size(record));
        record.name_offset = target_base + used;
        used += name_size(record);
    }
    if (skipped_slot < store->capacity) {
        store->records[skipped_slot].name_offset = target_base + used;
        store->records[skipped_slot].name_bytes = 0;
        store->records[skipped_slot].username_bytes = 0;
    }
    store->active_name_bank = target_bank;
    store->name_used = used;
    store->name_wasted = 0;
    saturating_increment(store->compactions);
    return true;
}

u32 empty_slot(const UserStore& store) noexcept {
    for (u32 slot = 0; slot < store.capacity; ++slot) {
        if (!store.records[slot].occupied) {
            return slot;
        }
    }
    return store.capacity;
}

u32 least_recent_slot(const UserStore& store, u32 skipped = ~0u) noexcept {
    u32 result = store.capacity;
    u32 oldest = ~0u;
    for (u32 slot = 0; slot < store.capacity; ++slot) {
        const UserRecord& record = store.records[slot];
        if (slot != skipped && record.occupied && !record.pinned && (result == store.capacity || record.last_seen < oldest)) {
            result = slot;
            oldest = record.last_seen;
        }
    }
    return result;
}

bool remove_slot(UserStore* store, u32 slot) noexcept {
    if (slot >= store->capacity || !store->records[slot].occupied) {
        return false;
    }
    bool found = false;
    const u32 order = find_order(*store, store->records[slot].id, &found);
    if (!found || store->order[order] != slot) {
        return false;
    }
    store->name_wasted += name_size(store->records[slot]);
    const u16 generation = next_generation(store->records[slot].generation);
    store->records[slot] = UserRecord{};
    store->records[slot].generation = generation;
    for (u32 index = order + 1; index < store->count; ++index) {
        store->order[index - 1] = store->order[index];
    }
    --store->count;
    store->order[store->count] = ~0u;
    return true;
}

UserStoreResult store_error(UserStore* store, UserStoreError error) noexcept {
    if (store) saturating_increment(store->rejected);
    return UserStoreResult{error, false, false, false};
}

bool avatar_hash_valid(Utf8View hash) noexcept {
    if (hash.bytes == 0) {
        return true;
    }
    const u32 begin = asset_hash_animated(hash) ? 2u : 0u;
    if (!hash.data || hash.bytes - begin != 32) {
        return false;
    }
    for (u32 index = begin; index < hash.bytes; ++index) {
        if (!ascii_is_hex_digit(hash.data[index])) {
            return false;
        }
    }
    return true;
}

bool initialize_store(UserStore* store, Slice<UserRecord> records, Slice<u32> order,
                      Slice<u8> double_buffered_names, bool zeroed) noexcept {
    if (!store || !records.data || records.count == 0 || records.count > 1'000'000 || !order.data ||
        order.count != records.count || !double_buffered_names.data || double_buffered_names.count < 2 ||
        (double_buffered_names.count & 1u) != 0) {
        return false;
    }
    *store = UserStore{};
    store->records = records.data;
    store->order = order.data;
    store->names = double_buffered_names.data;
    store->capacity = records.count;
    store->name_bank_bytes = double_buffered_names.count / 2;
    if (zeroed)
        return true;
    for (u32 index = 0; index < records.count; ++index) {
        records[index] = UserRecord{};
        order[index] = ~0u;
    }
    return true;
}
} // namespace

bool user_store_initialize(UserStore* store, Slice<UserRecord> records, Slice<u32> order,
                           Slice<u8> double_buffered_names) noexcept {
    return initialize_store(store, records, order, double_buffered_names, false);
}

bool user_store_initialize_sparse(UserStore* store, Slice<UserRecord> records, Slice<u32> order,
                                  Slice<u8> double_buffered_names) noexcept {
    return initialize_store(store, records, order, double_buffered_names, true);
}

// Only occupied records can be pinned (removal resets a record), so the
// ordered prefix covers them without touching unused slots.
void user_store_clear_pins(UserStore* store) noexcept {
    if (!store || !store->records || !store->order)
        return;
    ++store->pin_epoch;
    for (u32 index = 0; index < store->count; ++index)
        store->records[store->order[index]].pinned = false;
}
bool user_store_pin(UserStore* store, u64 id) noexcept {
    if (!store || !store->records || !store->order || !id) return false;
    bool found = false;
    const u32 order = find_order(*store, id, &found);
    if (!found) return false;
    store->records[store->order[order]].pinned = true;
    return true;
}

UserStoreResult user_store_upsert(UserStore* store, u64 id,
                                                 Utf8View display_name, Utf8View username) noexcept {
    if (!store || !store->records || !store->order || !store->names || id == 0 || !display_name.data ||
        display_name.bytes == 0) {
        return store_error(store, UserStoreError::invalid_input);
    }
    if (display_name.bytes > kUserDisplayNameMaximumBytes || username.bytes > kUserDisplayNameMaximumBytes) {
        return store_error(store, UserStoreError::name_too_large);
    }
    if (!utf8_validate(display_name).valid || !utf8_validate(username).valid) {
        return store_error(store, UserStoreError::invalid_utf8);
    }
    bool found = false;
    u32 order_index = find_order(*store, id, &found);
    if (found) {
        UserRecord& record = store->records[store->order[order_index]];
        if (!username.bytes) username = user_store_username(*store, record);
        if (display_name.bytes + username.bytes > store->name_bank_bytes)
            return store_error(store, UserStoreError::name_capacity);
        // Unchanged page arrivals only touch LRU state. Compare the separate
        // spans before staging borrowed bank bytes or scanning protected users.
        if (record.name_bytes == display_name.bytes && record.username_bytes == username.bytes &&
            bytes_equal(store->names + record.name_offset, display_name.data, display_name.bytes) &&
            bytes_equal(store->names + record.name_offset + record.name_bytes, username.data, username.bytes)) {
            saturating_increment(store->clock);
            record.last_seen = store->clock;
            return UserStoreResult{UserStoreError::none, false, false, false};
        }
    }
    const u16 display_bytes = static_cast<u16>(display_name.bytes), username_bytes = static_cast<u16>(username.bytes);
    // Inputs may borrow the current byte bank. Mutations still stage both
    // spans before eviction or compaction can invalidate their source bytes.
    char names[2 * kUserDisplayNameMaximumBytes];
    copy_bytes(names, display_name.data, display_name.bytes);
    copy_bytes(names + display_name.bytes, username.data, username.bytes);
    display_name = {names, display_name.bytes + username.bytes};
    if (display_name.bytes > store->name_bank_bytes) return store_error(store, UserStoreError::name_capacity);
    // Reject pressure that cannot be relieved before touching any identity or
    // byte-bank backing. Protected visible users must survive both record and
    // name-capacity exhaustion, including an oversized replacement name.
    const u32 existing_slot = found ? store->order[order_index] : store->capacity;
    const u32 available_slot = found || store->count == store->capacity ? store->capacity : empty_slot(*store);
    // If all live names plus this replacement fit, every protected subset
    // also fits. Inspect protected records only when names require eviction.
    const u32 capacity_live_bytes = store->name_used - store->name_wasted;
    const u32 capacity_replaced_bytes = found ? name_size(store->records[existing_slot]) : 0;
    if (display_name.bytes > store->name_bank_bytes - (capacity_live_bytes - capacity_replaced_bytes)) {
        u32 protected_bytes = 0;
        for (u32 protected_slot = 0; protected_slot < store->capacity; ++protected_slot) {
            const auto& protected_record = store->records[protected_slot];
            if (protected_slot != existing_slot && protected_record.occupied && protected_record.pinned)
                protected_bytes += name_size(protected_record);
        }
        if (protected_bytes > store->name_bank_bytes || display_name.bytes > store->name_bank_bytes - protected_bytes)
            return store_error(store, UserStoreError::name_capacity);
    }
    u32 first_victim = store->capacity;
    if (!found && available_slot == store->capacity) {
        first_victim = least_recent_slot(*store);
        if (first_victim == store->capacity)
            return store_error(store, UserStoreError::name_capacity);
    }
    saturating_increment(store->clock);
    bool evicted = false;
    u32 eviction_count = 0;
    u32 slot = found ? store->order[order_index] : available_slot;
    if (!found && slot == store->capacity) {
        // Only the store clock changed since preflight. Record ages and pins
        // still select the same first victim.
        slot = first_victim;
        if (slot == store->capacity || !remove_slot(store, slot)) {
            return store_error(store, UserStoreError::name_capacity);
        }
        evicted = true;
        ++eviction_count;
        order_index = find_order(*store, id, &found);
    }

    // READY can fill the byte bank before the record array (for example with
    // multibyte display names). Apply the same bounded LRU policy to both limits.
    while (display_name.bytes > store->name_bank_bytes -
        (store->name_used - store->name_wasted - (found ? name_size(store->records[slot]) : 0))) {
        const u32 victim = least_recent_slot(*store, found ? slot : ~0u);
        if (victim == store->capacity || !remove_slot(store, victim))
            return store_error(store, UserStoreError::name_capacity);
        ++eviction_count;
        evicted = true;
    }
    order_index = find_order(*store, id, &found);

    if (display_name.bytes > store->name_bank_bytes - store->name_used) {
        const u32 live_bytes = store->name_used - store->name_wasted;
        const u32 replaced_bytes = found ? name_size(store->records[slot]) : 0;
        if (replaced_bytes > live_bytes ||
            display_name.bytes > store->name_bank_bytes - (live_bytes - replaced_bytes) ||
            !compact_names(store, found ? slot : store->capacity)) {
            return store_error(store, UserStoreError::name_capacity);
        }
    }
    if (display_name.bytes > store->name_bank_bytes - store->name_used) {
        return store_error(store, UserStoreError::name_capacity);
    }
    const u32 name_offset = active_name_base(*store) + store->name_used;
    copy_bytes(store->names + name_offset, reinterpret_cast<const u8*>(display_name.data),
               display_name.bytes);
    store->name_used += display_name.bytes;

    UserRecord& record = store->records[slot];
    if (!found) {
        for (u32 index = store->count; index > order_index; --index) {
            store->order[index] = store->order[index - 1];
        }
        store->order[order_index] = slot;
        ++store->count;
        const u16 generation = next_generation(record.generation);
        record = UserRecord{};
        record.id = id;
        record.generation = generation;
        record.occupied = true;
    } else {
        store->name_wasted += name_size(record);
    }
    record.name_offset = name_offset;
    record.name_bytes = display_bytes;
    record.username_bytes = username_bytes;
    record.last_seen = store->clock;
    saturating_increment(record.revision);
    saturating_increment(store->revision);
    saturating_increment(store->mutations);
    for (u32 i = 0; i < eviction_count; ++i) saturating_increment(store->evictions);
    return UserStoreResult{UserStoreError::none, !found, true, evicted};
}

bool user_store_set_avatar(UserStore* store, u64 id, Utf8View avatar_hash) noexcept {
    if (!store || !store->records || !store->order || id == 0 || !avatar_hash_valid(avatar_hash)) {
        return false;
    }
    bool found = false;
    const u32 order = find_order(*store, id, &found);
    if (!found) {
        return false;
    }
    UserRecord& record = store->records[store->order[order]];
    bool equal = record.avatar_hash_bytes == avatar_hash.bytes;
    for (u32 index = 0; equal && index < avatar_hash.bytes; ++index) {
        equal = record.avatar_hash[index] == avatar_hash.data[index];
    }
    if (equal) {
        return true;
    }
    copy_bytes(record.avatar_hash, avatar_hash.data, avatar_hash.bytes);
    for (u32 index = avatar_hash.bytes; index < kUserAvatarHashMaximumBytes; ++index) {
        record.avatar_hash[index] = 0;
    }
    record.avatar_hash_bytes = static_cast<u8>(avatar_hash.bytes);
    saturating_increment(record.revision);
    saturating_increment(store->revision);
    saturating_increment(store->mutations);
    return true;
}

bool user_store_set_client_status(UserStore* store, u64 id, u8 client_status) noexcept {
    if (!store || !store->records || !store->order || !id) return false;
    bool found = false;
    const u32 order = find_order(*store, id, &found);
    if (!found) return false;
    UserRecord& record = store->records[store->order[order]];
    if (record.client_status == client_status) return false;
    record.client_status = client_status;
    return true;
}

u8 user_store_client_status(const UserStore& store, u64 id) noexcept {
    const UserRecord* record = user_store_find(store, id);
    return record ? record->client_status : 0;
}

const UserRecord* user_store_find(const UserStore& store, u64 id) noexcept {
    if (!store.records || !store.order || id == 0) {
        return nullptr;
    }
    bool found = false;
    const u32 order = find_order(store, id, &found);
    return found ? &store.records[store.order[order]] : nullptr;
}

Utf8View user_store_name(const UserStore& store, const UserRecord& record) noexcept {
    return record.occupied && record.name_bytes != 0 && store.names &&
                   record.name_offset + record.name_bytes <= store.name_bank_bytes * 2
               ? Utf8View{reinterpret_cast<const char*>(store.names + record.name_offset),
                          record.name_bytes}
               : Utf8View{};
}

Utf8View user_store_username(const UserStore& store, const UserRecord& record) noexcept {
    return record.occupied && store.names && record.name_offset + name_size(record) <= store.name_bank_bytes * 2
        ? Utf8View{reinterpret_cast<const char*>(store.names + record.name_offset + record.name_bytes), record.username_bytes} : Utf8View{};
}

Utf8View user_store_avatar_hash(const UserRecord& record) noexcept {
    return record.occupied && record.avatar_hash_bytes <= kUserAvatarHashMaximumBytes
               ? Utf8View{record.avatar_hash, record.avatar_hash_bytes}
               : Utf8View{};
}

u32 user_avatar_variant(Utf8View avatar_hash) noexcept {
    if (!avatar_hash_valid(avatar_hash) || avatar_hash.bytes == 0) {
        return 0;
    }
    u32 value = 2'166'136'261u;
    for (u32 index = 0; index < avatar_hash.bytes; ++index) {
        value ^= static_cast<u8>(avatar_hash.data[index]);
        value *= 16'777'619u;
    }
    return value == 0 ? 1u : value;
}

static_assert(sizeof(UserRecord) == 64);

} // namespace maicord
