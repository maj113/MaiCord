#include "maicord/model/guild_member_store.hpp"

#include "maicord/core/heap_sort.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/role_policy.hpp"

namespace maicord {
namespace {

constexpr u16 next_generation(u16 generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

bool status_valid(GuildMemberStatus status) noexcept {
    return status >= GuildMemberStatus::unknown && status <= GuildMemberStatus::offline;
}

u32 find_order(const GuildMemberStore& store, u64 guild_id, u64 user_id, bool* found) noexcept {
    u32 first = 0;
    u32 count = store.count;
    while (count != 0) {
        const u32 step = count / 2;
        const u32 current = first + step;
        const GuildMemberRecord& record = store.records[store.order[current]];
        if (record.guild_id < guild_id || (record.guild_id == guild_id && record.user_id < user_id)) {
            first = current + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    *found = first < store.count && store.records[store.order[first]].guild_id == guild_id &&
             store.records[store.order[first]].user_id == user_id;
    return first;
}

bool nickname_equal(const GuildMemberRecord& record, Utf8View nickname) noexcept {
    if (record.nickname_bytes != nickname.bytes) {
        return false;
    }
    return bytes_equal(record.nickname, nickname.data, nickname.bytes);
}

u32 empty_slot(const GuildMemberStore& store) noexcept {
    for (u32 slot = 0; slot < store.capacity; ++slot) {
        if (!store.records[slot].occupied) {
            return slot;
        }
    }
    return store.capacity;
}

// How much the list on screen needs the row at `index`: 2 inside the visible
// range, 1 inside a pinned page, 0 otherwise.
u32 row_interest(const GuildMemberStore& store, u64 guild_id, u64 list_id, u32 index) noexcept {
    if (guild_member_store_visible_index(store, guild_id, list_id, index))
        return 2;
    for (u32 pin = 0; pin < store.pin_count; ++pin) {
        const auto& range = store.pins[pin];
        if (range.guild_id == guild_id && range.list_id == list_id && index >= range.first &&
            index <= range.last)
            return 1;
    }
    return 0;
}
bool pinned(const GuildMemberStore& store, const GuildMemberRecord& record) noexcept {
    if (record.visible_pinned)
        return true;
    return record.list_index != ~0u &&
           row_interest(store, record.guild_id, record.list_id, record.list_index) != 0;
}
u32 least_recent_slot(const GuildMemberStore& store) noexcept {
    u32 result = store.capacity;
    bool result_pinned = true;
    u32 oldest = ~0u;
    for (u32 slot = 0; slot < store.capacity; ++slot) {
        const auto& record = store.records[slot];
        if (!record.occupied)
            continue;
        const bool keep = pinned(store, record);
        if (result == store.capacity || (!keep && result_pinned) ||
            (keep == result_pinned && record.last_seen < oldest)) {
            result = slot;
            oldest = record.last_seen;
            result_pinned = keep;
        }
    }
    return result_pinned ? store.capacity : result;
}

// Role overflow: each heavy member's whole list, contiguous, bump-allocated;
// released spans are reclaimed at the tail or by compaction.
u64* overflow_data(const GuildMemberStore& store) noexcept {
    return const_cast<u64*>(stable_storage_data<u64>(store.role_overflow.storage));
}
void overflow_shrink(GuildMemberStore* store) noexcept {
    auto& o = store->role_overflow;
    if (!o.storage.arena.base)
        return;
    if (o.wasted >= o.used)
        o.used = o.wasted = 0;
    stable_storage_shrink(&o.storage, o.used);
}
void overflow_release(GuildMemberStore* store, GuildMemberRecord& record) noexcept {
    if (!record.role_overflow)
        return;
    auto& o = store->role_overflow;
    const u32 offset = record.role_overflow - 1;
    record.role_overflow = 0;
    if (offset + record.role_count == o.used)
        o.used = offset;
    else
        o.wasted += record.role_count;
    if (o.wasted >= o.used)
        overflow_shrink(store);
}
// Slides live spans down in offset order and returns the freed pages.
void overflow_compact(GuildMemberStore* store) noexcept {
    auto& o = store->role_overflow;
    u32 slots[kGuildMemberRoleOverflowMembers];
    u32 count = 0;
    for (u32 slot = 0; slot < store->capacity; ++slot)
        if (store->records[slot].occupied && store->records[slot].role_overflow)
            slots[count++] = slot;
    const GuildMemberRecord* records = store->records;
    heap_sort(slots, count, [records](u32 left, u32 right) noexcept {
        return records[left].role_overflow < records[right].role_overflow;
    });
    u64* data = overflow_data(*store);
    u32 cursor = 0;
    for (u32 i = 0; i < count; ++i) {
        auto& record = store->records[slots[i]];
        const u32 offset = record.role_overflow - 1;
        if (offset != cursor)
            copy_bytes(data + cursor, data + offset, record.role_count * 8u);
        record.role_overflow = cursor + 1;
        cursor += record.role_count;
    }
    o.used = cursor;
    o.wasted = 0;
    saturating_increment(o.compactions);
    stable_storage_shrink(&o.storage, o.used);
}
// A span of `count` ids, or null when overflow is off or full.
u64* overflow_take(GuildMemberStore* store, u32 count, u32* offset) noexcept {
    auto& o = store->role_overflow;
    if (!o.limit || count > o.limit)
        return nullptr;
    if (!o.storage.arena.base &&
        !stable_storage_initialize(&o.storage, o.limit, 0, sizeof(u64), alignof(u64)))
        return nullptr;
    // Reuse released spans before committing more pages.
    if (o.used + count > o.storage.capacity && (o.wasted >= count || o.used + count > o.limit))
        overflow_compact(store);
    if (o.used + count > o.limit || !stable_storage_reserve(&o.storage, o.used + count))
        return nullptr;
    *offset = o.used;
    o.used += count;
    return overflow_data(*store) + *offset;
}

bool erase_slot(GuildMemberStore* store, u32 slot) noexcept {
    if (slot >= store->capacity || !store->records[slot].occupied) {
        return false;
    }
    const u64 guild_id = store->records[slot].guild_id;
    const u64 user_id = store->records[slot].user_id;
    bool found = false;
    const u32 order_index = find_order(*store, guild_id, user_id, &found);
    if (!found || store->order[order_index] != slot) {
        return false;
    }
    overflow_release(store, store->records[slot]);
    const u16 generation = next_generation(store->records[slot].generation);
    store->records[slot] = GuildMemberRecord{};
    store->records[slot].generation = generation;
    for (u32 index = order_index + 1; index < store->count; ++index) {
        store->order[index - 1] = store->order[index];
    }
    --store->count;
    store->order[store->count] = ~0u;
    return true;
}

void resolve_roles(const ChatStateStore& state, u64 guild_id, ConstSlice<u64> role_ids,
                   u64* group_role_id, u32* group_position, u64* color_role_id, u32* role_color) noexcept {
    const MemberRoleDisplay display = role_member_display(state, guild_id, role_ids);
    *color_role_id = display.color ? display.color->id : 0;
    *role_color = display.color ? display.color->color : 0;
    *group_role_id = display.hoist ? display.hoist->id : 0;
    *group_position = display.hoist ? display.hoist->position : 0;
}

// An incomplete record compares its inline prefix, so an unchanged list does
// not churn revisions while the overflow is off or full.
bool role_ids_equal(const GuildMemberStore& store, const GuildMemberRecord& record,
                    ConstSlice<u64> role_ids) noexcept {
    if (record.role_count != minimum<u32>(role_ids.count, kGuildMemberRoleMaximum))
        return false;
    const u64* held = record.role_overflow ? overflow_data(store) + (record.role_overflow - 1) : record.role_ids;
    const u32 compared =
        record.role_ids_complete ? record.role_count : minimum(role_ids.count, kGuildMemberRoleIdCapacity);
    for (u32 index = 0; index < compared; ++index)
        if (held[index] != role_ids[index])
            return false;
    return true;
}

void store_nickname(GuildMemberRecord& record, Utf8View nickname) noexcept {
    copy_bytes(record.nickname, nickname.data, nickname.bytes);
    for (u32 index = nickname.bytes; index < kGuildMemberNicknameMaximumBytes; ++index)
        record.nickname[index] = 0;
    record.nickname_bytes = static_cast<u16>(nickname.bytes);
}

void store_roles(GuildMemberStore* store, GuildMemberRecord& record, ConstSlice<u64> role_ids, u64 color_role_id,
                 u32 role_color) noexcept {
    const u32 count = minimum<u32>(role_ids.count, kGuildMemberRoleMaximum);
    // A list of the same length past 16 rewrites its span in place.
    if (record.role_overflow && record.role_count != count)
        overflow_release(store, record);
    record.color_role_id = color_role_id;
    record.role_color = role_color;
    record.role_count = static_cast<u16>(count);
    record.roles_known = true;
    const u32 retained = minimum(count, kGuildMemberRoleIdCapacity);
    for (u32 index = 0; index < kGuildMemberRoleIdCapacity; ++index)
        record.role_ids[index] = index < retained ? role_ids[index] : 0;
    record.role_ids_complete = role_ids.count <= kGuildMemberRoleIdCapacity;
    if (role_ids.count <= kGuildMemberRoleIdCapacity || role_ids.count > kGuildMemberRoleMaximum)
        return;
    u32 offset = record.role_overflow ? record.role_overflow - 1 : 0;
    u64* span = record.role_overflow ? overflow_data(*store) + offset : overflow_take(store, count, &offset);
    if (!span)
        return;
    copy_bytes(span, role_ids.data, count * 8u);
    record.role_overflow = offset + 1;
    record.role_ids_complete = true;
}

// Per-list positions (MemberListPositionTable). Scope slots index store.lists.
constexpr u8 kNoScope = 0xFF;
u8 scope_slot(const GuildMemberStore& store, u64 guild_id, u64 list_id) noexcept {
    if (!guild_id || !list_id)
        return kNoScope;
    for (u32 slot = 0; slot < kMemberListMaximumScopes; ++slot)
        if (store.lists[slot].guild_id == guild_id && store.lists[slot].list_id == list_id)
            return static_cast<u8>(slot);
    return kNoScope;
}
// The list on screen: its visible range or its subscribed (pinned) pages.
bool scope_shown(const GuildMemberStore& store, const MemberListScope& list) noexcept {
    if (!list.guild_id)
        return false;
    if (store.visible.guild_id == list.guild_id && store.visible.list_id == list.list_id)
        return true;
    for (u32 pin = 0; pin < store.pin_count; ++pin)
        if (store.pins[pin].guild_id == list.guild_id && store.pins[pin].list_id == list.list_id)
            return true;
    return false;
}
bool index_shown(const GuildMemberStore& store, u8 scope, u32 index) noexcept {
    const auto& list = store.lists[scope];
    return row_interest(store, list.guild_id, list.list_id, index) != 0;
}
void position_drop(GuildMemberStore* store, MemberListPosition* entry) noexcept {
    *entry = {};
    --store->positions.count;
}
void positions_clear(GuildMemberStore* store, u8 scope) noexcept {
    for (auto& entry : store->positions.entries)
        if (entry.occupied && entry.scope_slot == scope)
            position_drop(store, &entry);
}
void drop_scope(GuildMemberStore* store, u8 scope) noexcept;
// Room for one more row: the least recently viewed other list (not on screen)
// gives up all of its rows and its scope; showing it again subscribes afresh.
MemberListPosition* positions_evict_list(GuildMemberStore* store, u8 keep) noexcept {
    u32 held[kMemberListMaximumScopes]{};
    for (const auto& entry : store->positions.entries)
        if (entry.occupied && entry.scope_slot < kMemberListMaximumScopes)
            ++held[entry.scope_slot];
    u8 victim = kNoScope;
    for (u8 slot = 0; slot < kMemberListMaximumScopes; ++slot) {
        const auto& list = store->lists[slot];
        if (slot == keep || !held[slot] || !list.guild_id || scope_shown(*store, list))
            continue;
        const auto& best = store->lists[victim == kNoScope ? 0 : victim];
        if (victim == kNoScope || list.viewed < best.viewed ||
            (list.viewed == best.viewed && list.last_seen < best.last_seen))
            victim = slot;
    }
    if (victim == kNoScope)
        return nullptr;
    drop_scope(store, victim);
    saturating_increment(store->positions.evicted_lists);
    for (auto& entry : store->positions.entries)
        if (!entry.occupied)
            return &entry;
    return nullptr;
}
// A list holds one row per position and one position per user. True when
// the list's rows changed.
bool position_set(GuildMemberStore* store, u8 scope, u32 index, u64 user_id) noexcept {
    if (scope == kNoScope || index == ~0u || !user_id)
        return false;
    MemberListPosition* free_entry = nullptr;
    u32 held = 0;
    for (auto& entry : store->positions.entries) {
        if (entry.occupied && entry.scope_slot == scope) {
            if (entry.index == index && entry.user_id == user_id)
                return false;
            if (entry.index != index && entry.user_id != user_id) {
                ++held;
                continue;
            }
            position_drop(store, &entry);
        }
        if (!entry.occupied && !free_entry)
            free_entry = &entry;
    }
    if (held >= kMemberListPositionsPerList) {
        // Within its page budget a list keeps the pages on screen; the row
        // farthest from this one goes first.
        MemberListPosition* victim = nullptr;
        bool victim_shown = true;
        u32 distance = 0;
        for (auto& entry : store->positions.entries) {
            if (!entry.occupied || entry.scope_slot != scope)
                continue;
            const bool shown = index_shown(*store, scope, entry.index);
            const u32 away = entry.index > index ? entry.index - index : index - entry.index;
            if (!victim || (!shown && victim_shown) || (shown == victim_shown && away > distance)) {
                victim = &entry;
                victim_shown = shown;
                distance = away;
            }
        }
        if (victim) {
            position_drop(store, victim);
            free_entry = victim;
        }
    }
    if (!free_entry)
        free_entry = positions_evict_list(store, scope);
    if (!free_entry)
        return true;
    *free_entry = {user_id, index, scope, true};
    ++store->positions.count;
    return true;
}
void positions_invalidate(GuildMemberStore* store, u64 guild_id, u64 list_id, u32 first, u32 last) noexcept {
    for (auto& entry : store->positions.entries) {
        if (!entry.occupied || entry.scope_slot >= kMemberListMaximumScopes)
            continue;
        const auto& list = store->lists[entry.scope_slot];
        if (list.guild_id == guild_id && (!list_id || list.list_id == list_id) && entry.index >= first &&
            entry.index <= last)
            position_drop(store, &entry);
    }
}
void positions_shift(GuildMemberStore* store, u8 scope, u32 index, bool insert) noexcept {
    if (scope == kNoScope)
        return;
    for (auto& entry : store->positions.entries) {
        if (!entry.occupied || entry.scope_slot != scope)
            continue;
        if (insert && entry.index >= index) {
            if (entry.index == ~0u - 1)
                position_drop(store, &entry);
            else
                ++entry.index;
        } else if (!insert && entry.index > index) {
            --entry.index;
        }
    }
}
void positions_remove_user(GuildMemberStore* store, u64 guild_id, u64 user_id) noexcept {
    for (auto& entry : store->positions.entries)
        if (entry.occupied && entry.user_id == user_id && entry.scope_slot < kMemberListMaximumScopes &&
            store->lists[entry.scope_slot].guild_id == guild_id)
            position_drop(store, &entry);
}
void view_scope(GuildMemberStore* store, u64 guild_id, u64 list_id) noexcept {
    const u8 scope = scope_slot(*store, guild_id, list_id);
    if (scope == kNoScope)
        return;
    saturating_increment(store->clock);
    store->lists[scope].viewed = store->clock;
}

GuildMemberStoreResult fail(GuildMemberStore* store,
                                   GuildMemberStoreError error) noexcept {
    if (store)
        saturating_increment(store->rejected);
    return GuildMemberStoreResult{nullptr, error, false, false, false};
}

} // namespace

bool guild_member_store_observe_role(GuildMemberStore* store, const ChatStateStore& state,
                                             u64 guild_id, u64 role_id,
                                             const RoleRecord* previous) noexcept {
    if (!store || !guild_id || !role_id || !store->records)
        return false;
    const auto* current = chat_state_store_find_role(state, guild_id, role_id);
    if (previous && current && previous->position == current->position &&
        previous->color == current->color && previous->hoist == current->hoist)
        return false;
    const bool color_only = previous && current && previous->color && current->color &&
                            previous->position == current->position && previous->hoist == current->hoist;
    bool refresh = false;
    for (u32 index = 0; index < store->capacity; ++index) {
        auto& record = store->records[index];
        if (!record.occupied || record.guild_id != guild_id || !record.roles_known || !record.role_count)
            continue;
        if (color_only) {
            if (record.color_role_id != role_id || record.role_color == current->color)
                continue;
            record.role_color = current->color;
        } else {
            // Do not fabricate a lower assigned role or keep an obsolete winner.
            record.roles_known = false;
            record.color_role_id = record.group_role_id = 0;
            record.role_color = record.group_position = 0;
            refresh = true;
        }
        saturating_increment(record.revision);
        saturating_increment(store->revision);
        saturating_increment(store->mutations);
    }
    return refresh;
}

bool guild_member_store_initialize(GuildMemberStore* store,
                                           Slice<GuildMemberRecord> records,
                                           Slice<u32> order) noexcept {
    if (!store || !records.data || records.count == 0 || records.count > 1'000'000u || !order.data ||
        order.count != records.count) {
        return false;
    }
    *store = GuildMemberStore{};
    store->records = records.data;
    store->order = order.data;
    store->capacity = records.count;
    for (u32 index = 0; index < records.count; ++index) {
        records[index] = GuildMemberRecord{};
        order[index] = ~0u;
    }
    return true;
}

bool guild_member_store_enable_role_overflow(GuildMemberStore* store, u32 members_with_maximum) noexcept {
    if (!store || !store->records || store->capacity > kGuildMemberRoleOverflowMembers || !members_with_maximum ||
        members_with_maximum > store->capacity || store->role_overflow.storage.arena.base)
        return false;
    store->role_overflow.limit = members_with_maximum * kGuildMemberRoleMaximum;
    return true;
}

void guild_member_store_release(GuildMemberStore* store) noexcept {
    if (!store)
        return;
    for (u32 slot = 0; store->records && slot < store->capacity; ++slot) {
        auto& record = store->records[slot];
        if (record.role_overflow) {
            record.role_overflow = 0;
            record.role_ids_complete = false;
        }
    }
    stable_storage_release(&store->role_overflow.storage);
    store->role_overflow = {};
}

void guild_member_store_reset_lists(GuildMemberStore* store) noexcept {
    if (!store || !store->records || !store->order)
        return;
    bool changed = store->pin_count != 0 || store->visible.guild_id != 0;
    for (u32 slot = 0; slot < store->capacity; ++slot) {
        auto& member = store->records[slot];
        changed |= member.visible_pinned;
        member.visible_pinned = false;
        if (!member.occupied || (member.list_index == ~0u && !member.list_id))
            continue;
        member.list_index = ~0u;
        member.list_id = 0;
        saturating_increment(member.revision);
        changed = true;
    }
    for (auto& list : store->lists) {
        changed |= list.guild_id != 0;
        list = {};
    }
    for (auto& group : store->groups)
        group = {};
    for (auto& pin : store->pins)
        pin = {};
    store->pin_count = 0;
    store->visible = {};
    changed |= store->positions.count != 0;
    store->positions = {};
    saturating_increment_if(changed, store->revision);
    saturating_increment_if(changed, store->mutations);
}

void guild_member_store_retire_role_authority(GuildMemberStore* store) noexcept {
    if (!store || !store->records)
        return;
    bool changed = false;
    for (u32 i = 0; i < store->capacity; ++i) {
        auto& member = store->records[i];
        if (!member.occupied || !member.roles_known)
            continue;
        overflow_release(store, member);
        member.roles_known = false;
        member.role_count = 0;
        member.role_color = 0;
        member.color_role_id = 0;
        member.group_role_id = 0;
        member.group_position = 0;
        saturating_increment(member.revision);
        changed = true;
    }
    saturating_increment_if(changed, store->revision);
}

GuildMemberStoreResult guild_member_store_upsert(
    GuildMemberStore* store, const ChatStateStore& state, u64 guild_id, u64 user_id,
    Utf8View nickname, ConstSlice<u64> role_ids, bool roles_known, GuildMemberStatus status,
    u32 list_index, u64 list_id) noexcept {
    if (!store || !store->records || !store->order || guild_id == 0 || user_id == 0 ||
        (!nickname.data && nickname.bytes != 0) || (!role_ids.data && role_ids.count != 0) ||
        !status_valid(status)) {
        return fail(store, GuildMemberStoreError::invalid_input);
    }
    if (nickname.bytes > kGuildMemberNicknameMaximumBytes) {
        return fail(store, GuildMemberStoreError::nickname_too_large);
    }
    if (!utf8_validate(nickname).valid) {
        return fail(store, GuildMemberStoreError::invalid_utf8);
    }
    bool found = false;
    u32 order_index = find_order(*store, guild_id, user_id, &found);
    saturating_increment(store->clock);
    bool evicted = false;
    u32 slot = found ? store->order[order_index] : empty_slot(*store);
    if (!found && slot == store->capacity) {
        slot = least_recent_slot(*store);
        if (slot == store->capacity || !erase_slot(store, slot)) {
            return fail(store, GuildMemberStoreError::capacity);
        }
        evicted = true;
        order_index = find_order(*store, guild_id, user_id, &found);
    }

    GuildMemberRecord& record = store->records[slot];
    const bool nickname_changed = !found || !nickname_equal(record, nickname);
    u64 group_role_id = record.group_role_id;
    u32 group_position = record.group_position;
    u64 color_role_id = record.color_role_id;
    u32 role_color = record.role_color;
    if (roles_known) {
        resolve_roles(state, guild_id, role_ids, &group_role_id, &group_position, &color_role_id,
                      &role_color);
    }
    const GuildMemberStatus resolved_status =
        status == GuildMemberStatus::unknown && found ? record.status : status;
    u32 resolved_list_index = list_index == ~0u && found ? record.list_index : list_index;
    u64 resolved_list_id = list_id == 0 && found ? record.list_id : list_id;
    if (found && pinned(*store, record)) {
        auto candidate = record;
        candidate.list_id = resolved_list_id;
        candidate.list_index = resolved_list_index;
        if (!pinned(*store, candidate)) {
            resolved_list_id = record.list_id;
            resolved_list_index = record.list_index;
        }
    }
    const bool changed =
        !found || nickname_changed ||
        (roles_known && (!record.roles_known || record.group_role_id != group_role_id ||
                         record.color_role_id != color_role_id || record.role_color != role_color ||
                         !role_ids_equal(*store, record, role_ids))) ||
        record.status != resolved_status || record.list_index != resolved_list_index ||
        record.list_id != resolved_list_id;
    if (resolved_list_index != ~0u && list_index != ~0u && resolved_list_index == list_index &&
        resolved_list_id == list_id) {
        for (u32 other = 0; other < store->capacity; ++other) {
            auto& previous = store->records[other];
            if (other == slot || !previous.occupied || previous.guild_id != guild_id ||
                previous.list_id != list_id || previous.list_index != list_index)
                continue;
            previous.list_index = ~0u;
            saturating_increment(previous.revision);
            saturating_increment(store->revision);
            saturating_increment(store->mutations);
        }
        for (auto& list : store->lists) {
            if (list.guild_id != guild_id || list.list_id != list_id)
                continue;
            const auto scope = static_cast<u8>(&list - store->lists);
            for (auto& group : store->groups)
                if (group.scope_slot == scope && group.resident && group.index == list_index)
                    group.resident = false;
        }
    }
    if (!found) {
        for (u32 index = store->count; index > order_index; --index) {
            store->order[index] = store->order[index - 1];
        }
        store->order[order_index] = slot;
        ++store->count;
        const u16 generation = next_generation(record.generation);
        record = GuildMemberRecord{};
        record.guild_id = guild_id;
        record.user_id = user_id;
        record.generation = generation;
        record.occupied = true;
    }
    if (nickname_changed)
        store_nickname(record, nickname);
    if (roles_known) {
        record.group_role_id = group_role_id;
        record.group_position = group_position;
        store_roles(store, record, role_ids, color_role_id, role_color);
    }
    record.status = resolved_status;
    record.list_index = resolved_list_index;
    record.list_id = resolved_list_id;
    record.last_seen = store->clock;
    // The list's own rows take the position even when the record's list_index
    // stays with a pinned list.
    if (list_id && list_index != ~0u &&
        position_set(store, scope_slot(*store, guild_id, list_id), list_index, user_id) && !changed) {
        saturating_increment(store->revision);
        saturating_increment(store->mutations);
    }
    saturating_increment_if(changed, record.revision);
    saturating_increment_if(changed, store->revision);
    saturating_increment_if(changed, store->mutations);
    saturating_increment_if(evicted, store->evictions);
    return GuildMemberStoreResult{&record, GuildMemberStoreError::none, !found, changed,
                                         evicted};
}

GuildMemberInPlaceResult guild_member_store_update_in_place(
    GuildMemberStore* store, const ChatStateStore& state, u64 guild_id, u64 list_id, u32 list_index,
    u64 user_id, Utf8View nickname, ConstSlice<u64> role_ids, bool roles_known,
    GuildMemberStatus status) noexcept {
    GuildMemberInPlaceResult result{};
    if (!store || !store->records || !store->order || !guild_id || !list_id || !user_id ||
        list_index == ~0u || (!nickname.data && nickname.bytes) || (!role_ids.data && role_ids.count) ||
        !status_valid(status) || nickname.bytes > kGuildMemberNicknameMaximumBytes ||
        !utf8_validate(nickname).valid)
        return result;
    bool found = false;
    const u32 order_index = find_order(*store, guild_id, user_id, &found);
    if (!found)
        return result;
    GuildMemberRecord& record = store->records[store->order[order_index]];
    if (record.list_id != list_id || record.list_index != list_index)
        return result;
    u64 group_role_id = record.group_role_id, color_role_id = record.color_role_id;
    u32 group_position = record.group_position, role_color = record.role_color;
    if (roles_known)
        resolve_roles(state, guild_id, role_ids, &group_role_id, &group_position, &color_role_id, &role_color);
    const GuildMemberStatus resolved_status = status == GuildMemberStatus::unknown ? record.status : status;
    // A different sorted group or an offline/online move is structural.
    if (group_role_id != record.group_role_id || group_position != record.group_position ||
        (resolved_status == GuildMemberStatus::offline) != (record.status == GuildMemberStatus::offline))
        return result;
    result.applied = true;
    const bool nickname_changed = !nickname_equal(record, nickname);
    const bool roles_changed =
        roles_known && (!record.roles_known || record.color_role_id != color_role_id ||
                        record.role_color != role_color ||
                        !role_ids_equal(*store, record, role_ids));
    result.changed = nickname_changed || roles_changed || resolved_status != record.status;
    saturating_increment(store->clock);
    record.last_seen = store->clock;
    if (!result.changed)
        return result;
    if (nickname_changed)
        store_nickname(record, nickname);
    if (roles_changed)
        store_roles(store, record, role_ids, color_role_id, role_color);
    result.label_changed = nickname_changed;
    record.status = resolved_status;
    saturating_increment(record.revision);
    saturating_increment(store->presence_revision);
    saturating_increment(store->mutations);
    return result;
}

const GuildMemberRecord* guild_member_store_find(const GuildMemberStore& store,
                                                                u64 guild_id, u64 user_id) noexcept {
    if (!store.records || !store.order || guild_id == 0 || user_id == 0) {
        return nullptr;
    }
    bool found = false;
    const u32 order_index = find_order(store, guild_id, user_id, &found);
    return found ? &store.records[store.order[order_index]] : nullptr;
}

bool guild_member_store_set_status(GuildMemberStore* store, u64 guild_id, u64 user_id,
                                           GuildMemberStatus status) noexcept {
    if (!store || !store->records || !store->order || guild_id == 0 || user_id == 0 ||
        !status_valid(status))
        return false;
    bool found = false;
    const u32 index = find_order(*store, guild_id, user_id, &found);
    if (!found || status == GuildMemberStatus::unknown)
        return true;
    GuildMemberRecord& record = store->records[store->order[index]];
    if (record.status == status)
        return true;
    record.status = status;
    saturating_increment(record.revision);
    saturating_increment(store->presence_revision);
    saturating_increment(store->mutations);
    return true;
}

bool guild_member_store_role_ids_known(const GuildMemberRecord& record) noexcept {
    return record.occupied && record.roles_known && record.role_ids_complete &&
           (record.role_count <= kGuildMemberRoleIdCapacity || record.role_overflow);
}

ConstSlice<u64> guild_member_store_role_ids(const GuildMemberStore& store, const GuildMemberRecord& record) noexcept {
    if (!guild_member_store_role_ids_known(record))
        return {};
    return record.role_overflow ? ConstSlice<u64>{overflow_data(store) + (record.role_overflow - 1), record.role_count}
                                : ConstSlice<u64>{record.role_ids, record.role_count};
}

bool guild_member_store_set_timeout(GuildMemberStore* store, u64 guild_id, u64 user_id,
                                            u64 until_seconds) noexcept {
    if (!store || !store->records || !store->order || !guild_id || !user_id)
        return false;
    bool found = false;
    const u32 index = find_order(*store, guild_id, user_id, &found);
    if (!found)
        return false;
    auto& record = store->records[store->order[index]];
    const u32 until = static_cast<u32>(minimum<u64>(until_seconds, 0xFFFFFFFFull));
    if (record.communication_disabled_known && record.communication_disabled_until == until)
        return true;
    record.communication_disabled_until = until;
    record.communication_disabled_known = true;
    saturating_increment(record.revision);
    saturating_increment(store->revision);
    saturating_increment(store->mutations);
    return true;
}

Utf8View guild_member_store_nickname(const GuildMemberRecord& record) noexcept {
    return record.occupied && record.nickname_bytes <= kGuildMemberNicknameMaximumBytes
               ? Utf8View{record.nickname, record.nickname_bytes}
               : Utf8View{};
}

bool guild_member_store_remove(GuildMemberStore* store, u64 guild_id, u64 user_id) noexcept {
    if (!store || guild_id == 0 || user_id == 0) {
        return false;
    }
    bool found = false;
    const u32 order_index = find_order(*store, guild_id, user_id, &found);
    if (!found || !erase_slot(store, store->order[order_index])) {
        return false;
    }
    positions_remove_user(store, guild_id, user_id);
    saturating_increment(store->revision);
    saturating_increment(store->mutations);
    return true;
}

bool guild_member_store_invalidate_list_range(GuildMemberStore* store, u64 guild_id,
                                                      u32 first, u32 last, u64 list_id) noexcept {
    if (!store || !store->records || !store->order || guild_id == 0 || last < first) {
        return false;
    }
    bool changed = false;
    for (u32 order = 0; order < store->count; ++order) {
        const u32 slot = store->order[order];
        if (slot >= store->capacity || !store->records[slot].occupied) {
            return false;
        }
        GuildMemberRecord& member = store->records[slot];
        if (member.guild_id != guild_id || (list_id != 0 && member.list_id != list_id) ||
            member.list_index < first || member.list_index > last) {
            continue;
        }
        member.list_index = ~0u;
        saturating_increment(member.revision);
        changed = true;
    }
    for (auto& list : store->lists) {
        if (list.guild_id != guild_id || (list_id && list.list_id != list_id))
            continue;
        const auto scope = static_cast<u8>(&list - store->lists);
        for (auto& group : store->groups) {
            if (group.scope_slot == scope && group.resident && group.index >= first &&
                group.index <= last) {
                group.resident = false;
                changed = true;
            }
        }
    }
    const u32 positions_before = store->positions.count;
    positions_invalidate(store, guild_id, list_id, first, last);
    changed |= positions_before != store->positions.count;
    saturating_increment_if(changed, store->revision);
    saturating_increment_if(changed, store->mutations);
    return true;
}

void guild_member_store_clear_visible_range(GuildMemberStore* store) noexcept {
    if (store)
        store->visible = {};
}
bool guild_member_store_set_visible_range(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                                  u32 first, u32 last) noexcept {
    if (!store || !guild_id || !list_id || first == ~0u || last == ~0u || first > last ||
        last - first >= 128)
        return false;
    store->visible = {guild_id, list_id, first, last};
    view_scope(store, guild_id, list_id);
    return true;
}
bool guild_member_store_visible_index(const GuildMemberStore& store, u64 guild_id,
                                              u64 list_id, u32 index) noexcept {
    const auto& range = store.visible;
    return guild_id && range.guild_id == guild_id && range.list_id == list_id && index != ~0u &&
           index >= range.first && index <= range.last;
}
u32 guild_member_store_refresh_visible_users(const GuildMemberStore& members,
                                                     UserStore* users) noexcept {
    user_store_clear_pins(users);
    if (!members.records || !users)
        return 0;
    u32 count = 0;
    for (u32 slot = 0; slot < members.capacity; ++slot) {
        const auto& member = members.records[slot];
        if (member.occupied &&
            guild_member_store_visible_index(members, member.guild_id, member.list_id,
                                                     member.list_index) &&
            user_store_pin(users, member.user_id))
            ++count;
    }
    const u8 scope = scope_slot(members, members.visible.guild_id, members.visible.list_id);
    if (scope != kNoScope)
        for (const auto& entry : members.positions.entries)
            if (entry.occupied && entry.scope_slot == scope && entry.index >= members.visible.first &&
                entry.index <= members.visible.last) {
                // Rows the record pass above already pinned count once.
                const auto* user = user_store_find(*users, entry.user_id);
                if (user && !user->pinned && user_store_pin(users, entry.user_id))
                    ++count;
            }
    return count;
}

void guild_member_store_clear_pins(GuildMemberStore* store) noexcept {
    guild_member_store_clear_range_pins(store);
    guild_member_store_clear_user_pins(store);
}
void guild_member_store_clear_user_pins(GuildMemberStore* store) noexcept {
    if (!store || !store->records)
        return;
    ++store->user_pin_epoch;
    for (u32 slot = 0; slot < store->capacity; ++slot)
        store->records[slot].visible_pinned = false;
}
bool guild_member_store_pin_user(GuildMemberStore* store, u64 guild_id,
                                         u64 user_id) noexcept {
    if (!store || !store->records || !store->order || !guild_id || !user_id)
        return false;
    bool found = false;
    const u32 order = find_order(*store, guild_id, user_id, &found);
    if (!found)
        return false;
    store->records[store->order[order]].visible_pinned = true;
    return true;
}
bool guild_member_store_pin_range(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                          u32 first, u32 last) noexcept {
    if (!store || !guild_id || !list_id || first > last || store->pin_count >= 5)
        return false;
    store->pins[store->pin_count++] = {guild_id, list_id, first, last};
    view_scope(store, guild_id, list_id);
    return true;
}
const MemberListScope* guild_member_store_list(const GuildMemberStore& store,
                                                              u64 guild_id, u64 list_id) noexcept {
    for (const auto& list : store.lists)
        if (list.guild_id == guild_id && list.list_id == list_id && guild_id)
            return &list;
    return nullptr;
}
namespace {
MemberListScope* mutable_list(GuildMemberStore* store, u64 guild_id, u64 list_id) noexcept {
    if (!store)
        return nullptr;
    return const_cast<MemberListScope*>(guild_member_store_list(*store, guild_id, list_id));
}
// Withdraws a list: its rows, record positions and resident headers.
void drop_scope(GuildMemberStore* store, u8 scope) noexcept {
    auto& list = store->lists[scope];
    if (list.guild_id)
        guild_member_store_invalidate_list_range(store, list.guild_id, 0, ~0u, list.list_id);
    positions_clear(store, scope);
    for (auto& group : store->groups)
        if (group.scope_slot == scope)
            group.resident = false;
    list = {};
    saturating_increment(store->revision);
}
// A header row at index displaces the member recorded there. True when a
// member record or a position changed.
bool displace_header_row(GuildMemberStore* store, u64 guild_id, u64 list_id, u32 index) noexcept {
    bool changed = false;
    for (u32 order = 0; order < store->count; ++order) {
        auto& member = store->records[store->order[order]];
        if (member.guild_id == guild_id && member.list_id == list_id && member.list_index == index) {
            member.list_index = ~0u;
            saturating_increment(member.revision);
            changed = true;
        }
    }
    const u32 positions_before = store->positions.count;
    positions_invalidate(store, guild_id, list_id, index, index);
    return changed || positions_before != store->positions.count;
}
} // namespace
u32 guild_member_store_list_users(const GuildMemberStore& store, u64 guild_id, u64 list_id, u32 first,
                                  u32 last, Slice<u64> output) noexcept {
    const u8 scope = scope_slot(store, guild_id, list_id);
    u32 count = 0;
    if (scope == kNoScope || !output.data)
        return 0;
    for (const auto& entry : store.positions.entries)
        if (entry.occupied && entry.scope_slot == scope && entry.index >= first && entry.index <= last &&
            count < output.count)
            output[count++] = entry.user_id;
    return count;
}
bool guild_member_store_observe_list(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                             u32 member_count, u32 online_count, bool members_known,
                                             bool online_known, bool groups_known, u32 group_count,
                                             u32 logical_count) noexcept {
    if (!store || !store->records || !store->order || !guild_id || !list_id)
        return false;
    auto* list = mutable_list(store, guild_id, list_id);
    const bool fresh = !list;
    if (!list) {
        for (auto& candidate : store->lists) {
            if (!candidate.guild_id) {
                list = &candidate;
                break;
            }
            if (!scope_shown(*store, candidate) &&
                (!list || candidate.viewed < list->viewed ||
                 (candidate.viewed == list->viewed && candidate.last_seen < list->last_seen)))
                list = &candidate;
        }
        if (!list)
            return false;
        const auto scope = static_cast<u8>(list - store->lists);
        drop_scope(store, scope);
        list->guild_id = guild_id;
        list->list_id = list_id;
        // Positions recorded before the list existed seed its rows.
        for (u32 order = 0; order < store->count; ++order) {
            const auto& member = store->records[store->order[order]];
            if (member.guild_id == guild_id && member.list_id == list_id && member.list_index != ~0u)
                position_set(store, scope, member.list_index, member.user_id);
        }
    }
    saturating_increment(store->clock);
    list->last_seen = store->clock;
    list->groups_pending = groups_known;
    // Only projected facts publish a revision; a repeated header (every lazy
    // list packet carries counts and groups) is not a structural change.
    // online_count has no projection consumer.
    const MemberListScope before = *list;
    if (members_known) {
        list->member_count = member_count;
        list->member_count_known = true;
    }
    if (online_known) {
        list->online_count = online_count;
        list->online_count_known = true;
    }
    if (groups_known) {
        list->authoritative = true;
        list->groups_known = true;
        list->group_count = group_count;
        list->logical_count = logical_count;
        const auto scope = static_cast<u8>(list - store->lists);
        for (auto& group : store->groups)
            if (group.scope_slot == scope)
                group.summary_seen = false;
    } else if (!list->groups_known && members_known) {
        list->logical_count = maximum(list->logical_count, member_count);
    }
    const bool changed = fresh || before.member_count != list->member_count ||
                         before.member_count_known != list->member_count_known ||
                         before.authoritative != list->authoritative ||
                         before.groups_known != list->groups_known || before.group_count != list->group_count ||
                         before.logical_count != list->logical_count;
    saturating_increment_if(changed, store->revision);
    return true;
}
bool guild_member_store_group_summary(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                              u32 index, u64 group_id, u32 count) noexcept {
    auto* list = mutable_list(store, guild_id, list_id);
    if (!list || index == ~0u || count == ~0u)
        return false;
    // Every authoritative header position displaces a member, even when this
    // header itself is not retained in the bounded shared residency pool.
    bool changed = displace_header_row(store, guild_id, list_id, index);
    const auto scope = static_cast<u8>(list - store->lists);
    for (auto& group : store->groups) {
        if (group.scope_slot == scope && group.resident && group.id == group_id) {
            // summary_seen is reset by every packet header; it marks liveness, not change.
            changed |= group.index != index || group.count != count;
            group.index = index;
            group.count = count;
            group.summary_seen = true;
            break;
        }
    }
    saturating_increment_if(changed, store->revision);
    return true;
}

void guild_member_store_finish_groups(GuildMemberStore* store, u64 guild_id,
                                              u64 list_id) noexcept {
    auto* list = mutable_list(store, guild_id, list_id);
    if (!list)
        return;
    const auto scope = static_cast<u8>(list - store->lists);
    bool dropped = false;
    for (auto& group : store->groups)
        if (group.scope_slot == scope && group.resident && !group.summary_seen) {
            group.resident = false;
            dropped = true;
        }
    list->groups_pending = false;
    // A shrinking server list withdraws rows beyond its new logical end; that
    // call publishes its own revision when it withdraws anything.
    if (list->logical_count != ~0u)
        guild_member_store_invalidate_list_range(store, guild_id, list->logical_count, ~0u,
                                                         list_id);
    saturating_increment_if(dropped, store->revision);
}
bool guild_member_store_set_group(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                          u32 index, u64 group_id, u32 count, bool count_known) noexcept {
    auto* list = mutable_list(store, guild_id, list_id);
    if (!list || index == ~0u || count == ~0u || (!count_known && count))
        return false;
    const auto scope = static_cast<u8>(list - store->lists);
    MemberListGroupRecord* target = nullptr;
    u32 target_interest = 3;
    bool existing = false;
    for (auto& group : store->groups) {
        if (group.resident && group.scope_slot == scope && (group.index == index || group.id == group_id)) {
            target = &group;
            existing = true;
            break;
        }
        if (!group.resident) {
            if (!target || target->resident) {
                target = &group;
                target_interest = 0;
            }
            continue;
        }
        if (target && !target->resident)
            continue;
        const auto& owner = store->lists[group.scope_slot];
        const u32 priority = row_interest(*store, owner.guild_id, owner.list_id, group.index);
        if (!target || priority < target_interest ||
            (priority == target_interest && group.last_seen < target->last_seen)) {
            target = &group;
            target_interest = priority;
        }
    }
    // An unwanted late packet may still update list extent and cached identity,
    // but cannot steal resident headers needed by the current viewport/pages.
    if (!existing && target && target->resident &&
        target_interest > row_interest(*store, guild_id, list_id, index))
        return true;
    if (!target)
        return true;
    displace_header_row(store, guild_id, list_id, index);
    saturating_increment(store->clock);
    const u32 resolved_count = !count_known && existing && target->id == group_id ? target->count : count;
    *target = {group_id, index, resolved_count, true, false, scope, store->clock};
    guild_member_store_extend_list(store, guild_id, list_id, index);
    saturating_increment(store->revision);
    return true;
}

void guild_member_store_extend_list(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                            u32 index) noexcept {
    auto* list = mutable_list(store, guild_id, list_id);
    if (list && !list->groups_pending && index != ~0u)
        list->logical_count = maximum(list->logical_count, index + 1);
}
bool guild_member_store_shift_list(GuildMemberStore* store, u64 guild_id, u64 list_id,
                                           u32 index, bool insert) noexcept {
    if (!store || !store->records || !store->order || index == ~0u)
        return false;
    for (u32 order = 0; order < store->count; ++order) {
        auto& member = store->records[store->order[order]];
        if (member.guild_id != guild_id || member.list_id != list_id || member.list_index == ~0u)
            continue;
        if ((insert && member.list_index >= index) || (!insert && member.list_index > index)) {
            member.list_index = insert ? (member.list_index == ~0u - 1 ? ~0u : member.list_index + 1)
                                       : member.list_index - 1;
            saturating_increment(member.revision);
        }
    }
    auto* list = mutable_list(store, guild_id, list_id);
    if (list) {
        const auto scope = static_cast<u8>(list - store->lists);
        positions_shift(store, scope, index, insert);
        for (auto& group : store->groups) {
            if (!group.resident || group.scope_slot != scope)
                continue;
            if ((insert && group.index >= index) || (!insert && group.index > index)) {
                if (insert && group.index == ~0u - 1)
                    group.resident = false;
                else
                    group.index = insert ? group.index + 1 : group.index - 1;
            }
        }
        // Authoritative groups/counts supplied in this packet supersede deltas.
        if (!list->groups_pending) {
            if (insert)
                saturating_increment(list->logical_count);
            else if (list->logical_count)
                --list->logical_count;
        }
    }
    saturating_increment(store->revision);
    saturating_increment(store->mutations);
    return true;
}

static_assert(sizeof(GuildMemberRecord) == 336);

} // namespace maicord
