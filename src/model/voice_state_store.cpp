#include "maicord/model/voice_state_store.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
// Owned storage commits and returns whole pages of records.
constexpr u32 kPageRecords = 4096 / sizeof(VoiceState);
// Batches up to this size apply row by row.
constexpr u32 kBatchThreshold = 16;

u32 limit(const VoiceStateStore& store) noexcept {
    return store.owned.arena.base ? store.owned.reserved_capacity : store.records.count;
}
bool store_valid(const VoiceStateStore& store) noexcept {
    return store.records.data && (store.records.count || store.owned.arena.base) &&
           limit(store) <= kVoiceStateCapacity && store.count <= store.records.count;
}
u32 find_index(const VoiceStateStore& store, u64 guild_id, u64 user_id) noexcept {
    for (u32 i = 0; i < store.count; ++i)
        if (store.records[i].user_id == user_id && store.records[i].guild_id == guild_id)
            return i;
    return ~0u;
}
// First record whose channel is >= channel_id (or > with `after`).
u32 channel_bound(const VoiceStateStore& store, u64 channel_id, bool after) noexcept {
    u32 first = 0, count = store.count;
    while (count) {
        const u32 step = count / 2, current = first + step;
        const u64 id = store.records[current].channel_id;
        if (id < channel_id || (after && id == channel_id)) {
            first = current + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}
// Commits room for `required` records.
bool ensure(VoiceStateStore* store, u32 required) noexcept {
    if (required <= store->records.count)
        return true;
    if (!store->owned.arena.base || required > store->owned.reserved_capacity)
        return false;
    const u32 rounded = minimum(store->owned.reserved_capacity, (required + kPageRecords - 1) / kPageRecords * kPageRecords);
    if (!stable_storage_reserve(&store->owned, rounded))
        return false;
    store->records = {stable_storage_data<VoiceState>(store->owned), store->owned.capacity};
    return true;
}
// Returns pages well past the live records.
void trim(VoiceStateStore* store) noexcept {
    if (!store->owned.arena.base || store->owned.capacity <= store->count + 4 * kPageRecords)
        return;
    const u32 keep = (store->count + kPageRecords) / kPageRecords * kPageRecords;
    if (stable_storage_shrink(&store->owned, keep))
        store->records = {stable_storage_data<VoiceState>(store->owned), store->owned.capacity};
}
void erase_at(VoiceStateStore* store, u32 index) noexcept {
    copy_bytes(&store->records[index], &store->records[index + 1],
               (store->count - index - 1) * static_cast<u32>(sizeof(VoiceState)));
    --store->count;
}
u32 remove_guild(VoiceStateStore* store, u64 guild_id) noexcept {
    u32 kept = 0;
    for (u32 i = 0; i < store->count; ++i)
        if (store->records[i].guild_id != guild_id)
            store->records[kept++] = store->records[i];
    const u32 removed = store->count - kept;
    store->count = kept;
    return removed;
}
// At capacity the guild holding the most states goes, never the retained
// guild, private calls or `keep`. False when nothing can go.
bool evict(VoiceStateStore* store, u64 keep) noexcept {
    constexpr u32 kTally = 256;
    u64 ids[kTally];
    u32 counts[kTally], tallied = 0;
    for (u32 i = 0; i < store->count; ++i) {
        const u64 id = store->records[i].guild_id;
        if (!id || id == keep || id == store->retained_guild)
            continue;
        u32 slot = 0;
        while (slot < tallied && ids[slot] != id)
            ++slot;
        if (slot == tallied) {
            if (tallied == kTally)
                continue;
            ids[tallied] = id;
            counts[tallied++] = 0;
        }
        ++counts[slot];
    }
    u32 best = kTally;
    for (u32 slot = 0; slot < tallied; ++slot)
        if (best == kTally || counts[slot] > counts[best])
            best = slot;
    if (best == kTally)
        return false;
    saturating_accumulate(store->dropped, remove_guild(store, ids[best]));
    saturating_increment(store->evictions);
    saturating_increment(store->revision);
    return true;
}
// Room for `extra` more records, evicting whole guilds at capacity.
bool make_room(VoiceStateStore* store, u32 extra, u64 keep) noexcept {
    while (store->count + extra > limit(*store))
        if (!evict(store, keep))
            return false;
    return ensure(store, store->count + extra);
}
// Joins at the end of the channel's run.
void insert(VoiceStateStore* store, const VoiceState& state) noexcept {
    const u32 at = channel_bound(*store, state.channel_id, true);
    copy_bytes(&store->records[at + 1], &store->records[at],
               (store->count - at) * static_cast<u32>(sizeof(VoiceState)));
    store->records[at] = state;
    ++store->count;
}
VoiceState row_at(ConstSlice<u8> packed, u32 index) noexcept {
    VoiceState row;
    copy_bytes(&row, packed.data + static_cast<usize>(index) * sizeof(VoiceState), sizeof(VoiceState));
    if (!(row.flags & voice_request_to_speak))
        row.request_to_speak_s = 0;
    row.flags &= ~u32{voice_speaking};
    return row;
}
// Stable merge sort of `count` records by channel; `scratch` holds as many.
void sort_by_channel(VoiceState* data, VoiceState* scratch, u32 count) noexcept {
    VoiceState *from = data, *to = scratch;
    for (u32 width = 1; width < count; width *= 2) {
        for (u32 left = 0; left < count; left += 2 * width) {
            const u32 middle = minimum(left + width, count), right = minimum(left + 2 * width, count);
            u32 a = left, b = middle, out = left;
            while (a < middle && b < right)
                to[out++] = from[b].channel_id < from[a].channel_id ? from[b++] : from[a++];
            while (a < middle)
                to[out++] = from[a++];
            while (b < right)
                to[out++] = from[b++];
        }
        VoiceState* swap = from;
        from = to;
        to = swap;
    }
    if (from != data)
        copy_bytes(data, from, count * static_cast<u32>(sizeof(VoiceState)));
}
} // namespace

bool voice_state_store_initialize_sparse(VoiceStateStore* store, Slice<VoiceState> records) noexcept {
    if (!store || store->count || store->owned.arena.base || !records.data || !records.count ||
        records.count > kVoiceStateCapacity)
        return false;
    *store = {};
    store->records = records;
    return true;
}

bool voice_state_store_initialize_owned(VoiceStateStore* store, u32 reserved_capacity) noexcept {
    if (!store || store->count || store->owned.arena.base || !reserved_capacity ||
        reserved_capacity > kVoiceStateCapacity)
        return false;
    *store = {};
    if (!stable_storage_initialize(&store->owned, reserved_capacity, 0, sizeof(VoiceState), alignof(VoiceState)))
        return false;
    store->records = {stable_storage_data<VoiceState>(store->owned), 0};
    return true;
}

void voice_state_store_release(VoiceStateStore* store) noexcept {
    if (!store)
        return;
    stable_storage_release(&store->owned);
    *store = {};
}

bool voice_state_store_reset(VoiceStateStore* store) noexcept {
    if (!store || !store_valid(*store))
        return false;
    const bool changed = store->count != 0;
    store->count = 0;
    trim(store);
    saturating_increment_if(changed, store->revision);
    return true;
}

VoiceStateResult voice_state_store_apply(VoiceStateStore* store, u64 guild_id,
                                                        u64 channel_id, u64 user_id, u32 flags,
                                                        u32 request_to_speak_s) noexcept {
    if (!store || !store_valid(*store) || !user_id)
        return VoiceStateResult::invalid_input;
    if (!(flags & voice_request_to_speak))
        request_to_speak_s = 0;
    const u32 existing = find_index(*store, guild_id, user_id);
    if (!channel_id) {
        if (existing == ~0u)
            return VoiceStateResult::unchanged;
        erase_at(store, existing);
        saturating_increment(store->revision);
        return VoiceStateResult::changed;
    }
    if (existing != ~0u) {
        auto& record = store->records[existing];
        if (record.channel_id == channel_id) {
            flags = (flags & ~u32{voice_speaking}) | (record.flags & voice_speaking);
            if (record.flags == flags && record.request_to_speak_s == request_to_speak_s)
                return VoiceStateResult::unchanged;
            record.flags = flags;
            record.request_to_speak_s = request_to_speak_s;
            saturating_increment(store->revision);
            return VoiceStateResult::changed;
        }
        erase_at(store, existing);
    } else if (!make_room(store, 1, guild_id)) {
        saturating_increment(store->dropped);
        return VoiceStateResult::capacity;
    }
    insert(store, {guild_id, channel_id, user_id, flags & ~u32{voice_speaking}, request_to_speak_s});
    saturating_increment(store->revision);
    return VoiceStateResult::changed;
}

VoiceStateResult voice_state_store_apply_batch(VoiceStateStore* store, ConstSlice<u8> packed) noexcept {
    if (!store || !store_valid(*store) || (!packed.data && packed.count) || packed.count % sizeof(VoiceState))
        return VoiceStateResult::invalid_input;
    const u32 rows = static_cast<u32>(packed.count / sizeof(VoiceState));
    const u32 revision = store->revision;
    bool full = false;
    if (rows <= kBatchThreshold || !make_room(store, 3 * rows, rows ? row_at(packed, 0).guild_id : 0)) {
        for (u32 i = 0; i < rows; ++i) {
            const VoiceState row = row_at(packed, i);
            full = voice_state_store_apply(store, row.guild_id, row.channel_id, row.user_id, row.flags,
                                           row.request_to_speak_s) == VoiceStateResult::capacity ||
                   full;
        }
        return full ? VoiceStateResult::capacity
               : store->revision != revision ? VoiceStateResult::changed
                                             : VoiceStateResult::unchanged;
    }
    // Joins collect past every possible result (count + rows), then sort by
    // channel in arrival order and merge behind the existing records. A zero
    // channel marks a record that leaves.
    VoiceState* joins = &store->records[store->count + rows];
    VoiceState* scratch = joins + rows;
    u32 joined = 0;
    bool changed = false;
    for (u32 i = 0; i < rows; ++i) {
        const VoiceState row = row_at(packed, i);
        if (!row.user_id)
            continue;
        VoiceState* record = nullptr;
        for (u32 j = joined; j-- > 0 && !record;)
            if (joins[j].channel_id && joins[j].user_id == row.user_id && joins[j].guild_id == row.guild_id)
                record = &joins[j];
        if (!record) {
            const u32 at = find_index(*store, row.guild_id, row.user_id);
            record = at == ~0u || !store->records[at].channel_id ? nullptr : &store->records[at];
        }
        if (record && record->channel_id == row.channel_id) {
            const u32 flags = row.flags | (record->flags & voice_speaking);
            changed = changed || record->flags != flags || record->request_to_speak_s != row.request_to_speak_s;
            record->flags = flags;
            record->request_to_speak_s = row.request_to_speak_s;
            continue;
        }
        if (record) {
            record->channel_id = 0;
            changed = true;
        }
        if (row.channel_id) {
            joins[joined++] = row;
            changed = true;
        }
    }
    u32 kept = 0;
    for (u32 i = 0; i < store->count; ++i)
        if (store->records[i].channel_id)
            store->records[kept++] = store->records[i];
    u32 arrivals = 0;
    for (u32 j = 0; j < joined; ++j)
        if (joins[j].channel_id)
            joins[arrivals++] = joins[j];
    sort_by_channel(joins, scratch, arrivals);
    // Backward merge: on equal channels existing records stay first.
    u32 a = kept, b = arrivals, out = kept + arrivals;
    while (b) {
        if (a && store->records[a - 1].channel_id > joins[b - 1].channel_id)
            store->records[--out] = store->records[--a];
        else
            store->records[--out] = joins[--b];
    }
    store->count = kept + arrivals;
    trim(store);
    saturating_increment_if(changed, store->revision);
    return changed ? VoiceStateResult::changed : VoiceStateResult::unchanged;
}

VoiceStateResult voice_state_store_clear_guild(VoiceStateStore* store,
                                                              u64 guild_id) noexcept {
    if (!store || !store_valid(*store))
        return VoiceStateResult::invalid_input;
    if (!remove_guild(store, guild_id))
        return VoiceStateResult::unchanged;
    trim(store);
    saturating_increment(store->revision);
    return VoiceStateResult::changed;
}

const VoiceState* voice_state_store_find(const VoiceStateStore& store, u64 guild_id,
                                                        u64 user_id) noexcept {
    if (!store_valid(store))
        return nullptr;
    const u32 index = find_index(store, guild_id, user_id);
    return index == ~0u ? nullptr : &store.records[index];
}

u32 voice_state_store_channel_count(const VoiceStateStore& store, u64 channel_id) noexcept {
    if (!store_valid(store) || !channel_id)
        return 0;
    return channel_bound(store, channel_id, true) - channel_bound(store, channel_id, false);
}

const VoiceState* voice_state_store_channel_at(const VoiceStateStore& store,
                                                              u64 channel_id, u32 index) noexcept {
    if (!store_valid(store) || !channel_id)
        return nullptr;
    const u32 first = channel_bound(store, channel_id, false) + index;
    return first < store.count && first >= index && store.records[first].channel_id == channel_id
               ? &store.records[first]
               : nullptr;
}

bool voice_state_store_set_speaking(VoiceStateStore* store, u64 channel_id,
                                            ConstSlice<u64> users) noexcept {
    if (!store || !store_valid(*store))
        return false;
    bool changed = false;
    const u32 first = channel_id ? channel_bound(*store, channel_id, false) : store->count;
    const u32 last = channel_id ? channel_bound(*store, channel_id, true) : store->count;
    for (u32 i = 0; i < store->count; ++i) {
        auto& record = store->records[i];
        bool speaking = false;
        for (u32 k = 0; i >= first && i < last && k < users.count && !speaking; ++k)
            speaking = users[k] == record.user_id;
        const u32 flags = speaking ? record.flags | voice_speaking : record.flags & ~u32{voice_speaking};
        changed = changed || flags != record.flags;
        record.flags = flags;
    }
    saturating_increment_if(changed, store->revision);
    return changed;
}

} // namespace maicord
