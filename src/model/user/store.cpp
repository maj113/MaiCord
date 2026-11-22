#include "maicord/model/user/store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat/asset_hash.hpp"
#include "maicord/model/chat/sorted_search.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/virtual_memory.hpp"

namespace maicord {
namespace {

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

// A shared username is the display name's bytes: the bank holds them once. Inline: name lookups read both.
inline u32 name_size(const UserRecord& record) noexcept {
    return record.name_bytes + (record.username_shared ? 0u : u32{record.username_bytes});
}
inline u32 username_offset(const UserRecord& record) noexcept {
    return record.name_offset + (record.username_shared ? 0u : u32{record.name_bytes});
}
// Name compaction, storage commits and initialization are rare: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
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

usize page_round(usize bytes) noexcept {
    const usize page = pages_page_size();
    return (bytes + page - 1) & ~(page - 1);
}

// Commits owned records through `required` slots, in steps of kUserStoreCommitUsers, and the order entries in steps
// of 16 KiB (4,096 users), so a step costs one system call. Caller storage never asks: all of it is committed.
inline constexpr u32 kOrderCommitUsers = 4096;
bool commit_slots(UserStore* store, u32 required) noexcept {
    const u32 from = store->committed;
    const u32 to = minimum(store->capacity,
                           (required + kUserStoreCommitUsers - 1) / kUserStoreCommitUsers * kUserStoreCommitUsers);
    const u32 order_from = (from + kOrderCommitUsers - 1) / kOrderCommitUsers * kOrderCommitUsers;
    const u32 order_to = minimum(store->capacity, (to + kOrderCommitUsers - 1) / kOrderCommitUsers * kOrderCommitUsers);
    if (!store->reservation || !pages_commit_at(store->records + from, usize{to - from} * sizeof(UserRecord)) ||
        (order_to > order_from &&
         !pages_commit_at(store->order + order_from, usize{order_to - order_from} * sizeof(u32))))
        return false;
    store->committed = to;
    return true;
}

// Commits the first `required` bytes of a name bank, in steps of kUserStoreCommitNameBytes. False past the bank or
// when the system refuses.
bool commit_names(UserStore* store, u32 bank, u32 required) noexcept {
    u32& committed = store->name_committed[bank];
    if (required <= committed)
        return true;
    if (!store->reservation || required > store->name_bank_bytes)
        return false;
    const u32 to = minimum(store->name_bank_bytes, (required + kUserStoreCommitNameBytes - 1) /
                                                       kUserStoreCommitNameBytes * kUserStoreCommitNameBytes);
    if (!pages_commit_at(store->names + usize{bank} * store->name_bank_bytes + committed, to - committed))
        return false;
    committed = to;
    return true;
}

// Returns the pages of an owned bank that a compaction left. The next compaction into it commits them again.
void release_names(UserStore* store, u32 bank) noexcept {
    if (store->reservation && store->name_committed[bank] &&
        pages_decommit(store->names + usize{bank} * store->name_bank_bytes, store->name_committed[bank]))
        store->name_committed[bank] = 0;
}
#pragma clang attribute pop

// Slots at and above slot_extent are free.
u32 empty_slot(const UserStore& store) noexcept {
    for (u32 slot = store.free_hint; slot < store.slot_extent; ++slot) {
        if (!store.records[slot].occupied) {
            return slot;
        }
    }
    return store.slot_extent;
}

u32 least_recent_slot(const UserStore& store, u32 skipped = ~0u) noexcept {
    u32 result = store.capacity;
    u32 oldest = ~0u;
    for (u32 slot = 0; slot < store.slot_extent; ++slot) {
        const UserRecord& record = store.records[slot];
        if (slot != skipped && record.occupied && !user_store_pinned(store, record) &&
            (result == store.capacity || record.last_seen < oldest)) {
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
    reset_value<UserRecord>(&store->records[slot]);
    store->records[slot].generation = generation;
    store->free_hint = minimum(store->free_hint, slot);
    sorted_erase_gap(store->order, store->count, order);
    --store->count;
    store->order[store->count] = ~0u;
    return true;
}

UserStoreResult store_error(UserStore* store, UserStoreError error) noexcept {
    if (store) saturating_increment(store->rejected);
    return UserStoreResult{error, false, false};
}

#include "../chat/asset_hash_validation.inc"

} // namespace

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool user_store_initialize(UserStore* store, Slice<UserRecord> records, Slice<u32> order,
                           Slice<u8> double_buffered_names) noexcept {
    if (!store || !records.data || records.count == 0 || records.count > 1'000'000 || !order.data ||
        order.count != records.count || !double_buffered_names.data || double_buffered_names.count < 2 ||
        (double_buffered_names.count & 1u) != 0) {
        return false;
    }
    reset_value<UserStore>(store);
    store->pin_mark = 1;
    store->records = records.data;
    store->order = order.data;
    store->names = double_buffered_names.data;
    store->capacity = store->committed = records.count;
    store->name_bank_bytes = store->name_committed[0] = store->name_committed[1] = double_buffered_names.count / 2;
    for (u32 index = 0; index < records.count; ++index) {
        reset_value<UserRecord>(&records[index]);
        order[index] = ~0u;
    }
    return true;
}

bool user_store_initialize_owned(UserStore* store, u32 capacity, u32 name_bank_bytes) noexcept {
    if (capacity == 0 || capacity > 1'000'000 || name_bank_bytes == 0 || name_bank_bytes > (1u << 30))
        return false;
    // Each region starts on a page, so a bank's pages return to the system alone.
    const usize bank = page_round(name_bank_bytes);
    const usize order_at = page_round(usize{capacity} * sizeof(UserRecord));
    const usize names_at = order_at + page_round(usize{capacity} * sizeof(u32));
    auto* const base = static_cast<u8*>(pages_reserve(names_at + 2 * bank));
    if (!base)
        return false;
    reset_value<UserStore>(store);
    store->pin_mark = 1;
    store->records = reinterpret_cast<UserRecord*>(base);
    store->order = reinterpret_cast<u32*>(base + order_at);
    store->names = base + names_at;
    store->reservation = base;
    store->capacity = capacity;
    store->name_bank_bytes = static_cast<u32>(bank);
    return true;
}

void user_store_release(UserStore* store) noexcept {
    user_appearance_release(&store->appearance);
    user_activity_release(&store->activities);
    if (store->reservation)
        pages_release(store->reservation);
    reset_value<UserStore>(store);
}
#pragma clang attribute pop

// The next mark matches no record. Once each 255 clears the marks start again at 1, so the marks of the last
// 255 clears go first: only occupied records hold one (removal resets a record), and they lie below slot_extent.
void user_store_clear_pins(UserStore* store) noexcept {
    if (!store)
        return;
    ++store->pin_epoch;
    if (++store->pin_mark == 0) {
        store->pin_mark = 1;
        for (u32 slot = 0; slot < store->slot_extent; ++slot)
            store->records[slot].pin_mark = 0;
    }
}
bool user_store_pin(UserStore* store, u64 id) noexcept {
    if (!store || !id) return false;
    bool found = false;
    const u32 order = find_order(*store, id, &found);
    if (!found) return false;
    store->records[store->order[order]].pin_mark = store->pin_mark;
    return true;
}

UserStoreResult user_store_upsert(UserStore* store, u64 id,
                                                 const Utf8View& display_name_view, const Utf8View& username_view) noexcept {
    Utf8View display_name = display_name_view, username = username_view; // the store is written below
    if (!store || id == 0 || !display_name.data || display_name.bytes == 0) {
        return store_error(store, UserStoreError::invalid_input);
    }
    bool found = false;
    u32 order_index = find_order(*store, id, &found);
    if (found) {
        UserRecord& record = store->records[store->order[order_index]];
        // History pages repeat their authors: names equal to the stored,
        // already validated ones only touch LRU state and need no checks.
        const char* stored_username = reinterpret_cast<const char*>(store->names + username_offset(record));
        const Utf8View compared = username.bytes ? username : Utf8View{stored_username, record.username_bytes};
        if (record.name_bytes == display_name.bytes && record.username_bytes == compared.bytes &&
            bytes_equal(store->names + record.name_offset, display_name.data, display_name.bytes) &&
            bytes_equal(stored_username, compared.data, compared.bytes)) {
            saturating_increment(store->clock);
            record.last_seen = store->clock;
            return UserStoreResult{UserStoreError::none, false, false};
        }
    }
    if (display_name.bytes > kUserDisplayNameMaximumBytes || username.bytes > kUserDisplayNameMaximumBytes) {
        return store_error(store, UserStoreError::name_too_large);
    }
    if (!utf8_validate(display_name).valid || !utf8_validate(username).valid) {
        return store_error(store, UserStoreError::invalid_utf8);
    }
    if (found && !username.bytes)
        username = user_store_username(*store, store->records[store->order[order_index]]);
    // A user without a global name sends the username as the display name too: the bank holds it once.
    const bool shared = username.bytes == display_name.bytes && bytes_equal(username.data, display_name.data, username.bytes);
    const u8 display_bytes = static_cast<u8>(display_name.bytes), username_bytes = static_cast<u8>(username.bytes);
    // Inputs may borrow the current byte bank. Mutations still stage both
    // spans before eviction or compaction can invalidate their source bytes.
    char names[2 * kUserDisplayNameMaximumBytes];
    copy_bytes(names, display_name.data, display_name.bytes);
    const u32 stored_username_bytes = shared ? 0 : username.bytes;
    copy_bytes(names + display_name.bytes, username.data, stored_username_bytes);
    display_name = {names, display_name.bytes + stored_username_bytes};
    if (display_name.bytes > store->name_bank_bytes) return store_error(store, UserStoreError::name_capacity);
    // Reject pressure that cannot be relieved before touching any identity or
    // byte-bank backing. Protected visible users must survive both record and
    // name-capacity exhaustion, including an oversized replacement name.
    const u32 existing_slot = found ? store->order[order_index] : store->capacity;
    const u32 available_slot = found || store->count == store->capacity ? store->capacity : empty_slot(*store);
    // Owned storage commits records as the first free slot reaches the committed end.
    if (available_slot >= store->committed && available_slot != store->capacity &&
        !commit_slots(store, available_slot + 1))
        return store_error(store, UserStoreError::memory);
    // If all live names plus this replacement fit, every protected subset
    // also fits. Inspect protected records only when names require eviction.
    const u32 capacity_live_bytes = store->name_used - store->name_wasted;
    const u32 capacity_replaced_bytes = found ? name_size(store->records[existing_slot]) : 0;
    if (display_name.bytes > store->name_bank_bytes - (capacity_live_bytes - capacity_replaced_bytes)) {
        u32 protected_bytes = 0;
        for (u32 protected_slot = 0; protected_slot < store->slot_extent; ++protected_slot) {
            const auto& protected_record = store->records[protected_slot];
            if (protected_slot != existing_slot && protected_record.occupied &&
                user_store_pinned(*store, protected_record))
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
        if (!remove_slot(store, slot)) {
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
        // The eviction moved the order entries after the victim.
        order_index = find_order(*store, id, &found);
    }

    // A name that passes the committed end of the bank grows the bank. A full bank, or an owned bank that holds more
    // dead names than live ones, first compacts into the other bank, and the bank it leaves returns its pages.
    if (display_name.bytes > store->name_committed[store->active_name_bank] - store->name_used) {
        const u32 live_bytes = store->name_used - store->name_wasted;
        if (display_name.bytes > store->name_bank_bytes - store->name_used || store->name_wasted > live_bytes) {
            const u32 replaced_bytes = found ? name_size(store->records[slot]) : 0;
            if (replaced_bytes > live_bytes ||
                display_name.bytes > store->name_bank_bytes - (live_bytes - replaced_bytes))
                return store_error(store, UserStoreError::name_capacity);
            const u8 left = store->active_name_bank;
            if (!commit_names(store, left ^ 1u, live_bytes - replaced_bytes + display_name.bytes))
                return store_error(store, UserStoreError::memory);
            if (!compact_names(store, found ? slot : store->capacity))
                return store_error(store, UserStoreError::name_capacity);
            release_names(store, left);
        }
        if (!commit_names(store, store->active_name_bank, store->name_used + display_name.bytes))
            return store_error(store, UserStoreError::memory);
    }
    const u32 name_offset = active_name_base(*store) + store->name_used;
    copy_bytes(store->names + name_offset, reinterpret_cast<const u8*>(display_name.data),
               display_name.bytes);
    store->name_used += display_name.bytes;

    UserRecord& record = store->records[slot];
    if (!found) {
        sorted_insert_gap(store->order, store->count, order_index);
        store->order[order_index] = slot;
        ++store->count;
        const u16 generation = next_generation(record.generation);
        reset_value<UserRecord>(&record);
        record.id = id;
        record.generation = generation;
        record.occupied = true;
        if (slot == store->free_hint)
            ++store->free_hint;
        store->slot_extent = maximum(store->slot_extent, slot + 1);
    } else {
        store->name_wasted += name_size(record);
    }
    record.name_offset = name_offset;
    record.name_bytes = display_bytes;
    record.username_bytes = username_bytes;
    record.username_shared = shared;
    record.last_seen = store->clock;
    saturating_increment(record.revision);
    saturating_increment(store->revision);
    saturating_increment(store->mutations);
    store->evictions = eviction_count > ~u32{0} - store->evictions ? ~u32{0} : store->evictions + eviction_count;
    return UserStoreResult{UserStoreError::none, !found, evicted};
}

bool user_store_set_avatar(UserStore* store, u64 id, const Utf8View& avatar_view) noexcept {
    const Utf8View avatar_hash = avatar_view; // the store is written below
    if (!store || id == 0 || !asset_hash_valid(avatar_hash)) {
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
    if (!store || !id) return false;
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

bool user_store_clear_client_status(UserStore* store) noexcept {
    if (!store) return false;
    bool changed = false;
    for (u32 slot = 0; slot < store->slot_extent; ++slot) {
        UserRecord& record = store->records[slot];
        if (record.client_status) {
            record.client_status = 0;
            changed = true;
        }
    }
    return changed;
}

const UserRecord* user_store_find(const UserStore& store, u64 id) noexcept {
    if (id == 0) {
        return nullptr;
    }
    bool found = false;
    const u32 order = find_order(store, id, &found);
    return found ? &store.records[store.order[order]] : nullptr;
}

Utf8View user_store_name(const UserStore& store, const UserRecord& record) noexcept {
    return record.occupied && record.name_bytes != 0
               ? Utf8View{reinterpret_cast<const char*>(store.names + record.name_offset),
                          record.name_bytes}
               : Utf8View{};
}

Utf8View user_store_username(const UserStore& store, const UserRecord& record) noexcept {
    return record.occupied
        ? Utf8View{reinterpret_cast<const char*>(store.names + username_offset(record)), record.username_bytes} : Utf8View{};
}

Utf8View user_store_avatar_hash(const UserRecord& record) noexcept {
    return record.occupied
               ? Utf8View{record.avatar_hash, record.avatar_hash_bytes}
               : Utf8View{};
}

u32 user_avatar_variant(const Utf8View& avatar_hash) noexcept {
    if (!asset_hash_valid(avatar_hash) || avatar_hash.bytes == 0) {
        return 0;
    }
    return asset_hash_variant(avatar_hash);
}

static_assert(sizeof(UserRecord) == 64);

} // namespace maicord
