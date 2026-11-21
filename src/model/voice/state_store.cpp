#include "maicord/model/voice/state_store.hpp"

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
// The records of channel_id: the first one (or where it would go) and how many there are.
struct ChannelRun {
    u32 first, count;
};
// The first of `count` records whose channel_id - offset is not below limit. Branch-free (Khuong and Morin, "Array
// layouts for comparison-based searching", 2017): each step takes its half with a conditional move, so no step
// mispredicts.
u32 bound(const VoiceState* records, u32 count, u64 offset, u64 limit) noexcept {
    if (!count)
        return 0;
    const VoiceState* base = records;
    for (u32 n = count; n > 1;) {
        const u32 half = n / 2;
        base = base[half].channel_id - offset < limit ? base + half : base;
        n -= half;
    }
    return static_cast<u32>(base - records) + (base->channel_id - offset < limit);
}
ChannelRun channel_run(const VoiceStateStore& store, u64 channel_id) noexcept {
    const VoiceState* records = store.records.data;
    const u32 first = bound(records, store.count, 0, channel_id);
    // A channel holds few users. An exponential search from the first record finds the end of the run in about
    // 2 log2(users) probes, where a second binary search takes log2(records). From `first` on no channel is below
    // channel_id, so a difference below 1 marks the run.
    const u32 left = store.count - first;
    u32 low = 0, high = 1;
    while (high <= left && records[first + high - 1].channel_id == channel_id) {
        low = high;
        high *= 2;
    }
    const u32 end = minimum(high - 1, left);
    return {first, low + bound(records + first + low, end - low, channel_id, 1)};
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
// Moves the records whose `field` is not `value` to the front, in order. The records before the first that goes stay
// where they are, unwritten.
u32 compact(VoiceState* records, u32 count, u64 VoiceState::*field, u64 value) noexcept {
    u32 kept = 0;
    while (kept < count && records[kept].*field != value)
        ++kept;
    for (u32 i = kept; i < count; ++i)
        if (records[i].*field != value)
            records[kept++] = records[i];
    return kept;
}
u32 remove_guild(VoiceStateStore* store, u64 guild_id) noexcept {
    const u32 kept = compact(store->records.data, store->count, &VoiceState::guild_id, guild_id);
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
    remove_guild(store, ids[best]);
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
inline void insert(VoiceStateStore* store, const VoiceState& state) noexcept {
    const ChannelRun run = channel_run(*store, state.channel_id);
    const u32 at = run.first + run.count;
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
u64 row_field(ConstSlice<u8> packed, u32 index, usize offset) noexcept {
    u64 value;
    copy_bytes(&value, packed.data + static_cast<usize>(index) * sizeof(VoiceState) + offset, sizeof(value));
    return value;
}
// The slot of (guild_id, user_id) in a batch's open-addressing table of mask + 1 entries. An entry is one above
// the first row with its key, zero when empty. The start is the top bits of a multiplicative hash of the user.
u32 key_slot(const u32* table, u32 mask, u32 shift, ConstSlice<u8> packed, u64 guild_id, u64 user_id) noexcept {
    constexpr usize user = __builtin_offsetof(VoiceState, user_id), guild = __builtin_offsetof(VoiceState, guild_id);
    u32 slot = static_cast<u32>(user_id * 0x9e3779b97f4a7c15ull >> shift);
    while (table[slot] && (row_field(packed, table[slot] - 1, user) != user_id ||
                           row_field(packed, table[slot] - 1, guild) != guild_id))
        slot = (slot + 1) & mask;
    return slot;
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

bool voice_state_store_initialize_owned(VoiceStateStore* store, u32 reserved_capacity) noexcept {
    if (!store || store->count || store->owned.arena.base || !reserved_capacity ||
        reserved_capacity > kVoiceStateCapacity)
        return false;
    reset_value(store);
    if (!stable_storage_initialize(&store->owned, reserved_capacity, 0, sizeof(VoiceState), alignof(VoiceState)))
        return false;
    store->records = {stable_storage_data<VoiceState>(store->owned), 0};
    return true;
}

void voice_state_store_release(VoiceStateStore* store) noexcept {
    if (!store)
        return;
    stable_storage_release(&store->owned);
    reset_value(store);
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
    if (rows <= kBatchThreshold || !make_room(store, 3 * rows, row_at(packed, 0).guild_id)) {
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
    const u32 count = store->count;
    VoiceState* joins = &store->records[count + rows];
    VoiceState* scratch = joins + rows;
    // Until the merge, the room of `rows` records after the existing ones holds the key table of the batch (at most
    // half full: below 16 * rows bytes), each row's first row with its key, and where the state of that key is now
    // (an index of `records`, ~0u for none). One pass over the store finds the existing states. A batch is mostly
    // of one guild, so the pass hashes only the records inside the range of the batch's guilds.
    const u32 bits = 32 - static_cast<u32>(__builtin_clz(2 * rows - 1)), mask = (1u << bits) - 1, shift = 64 - bits;
    u32* table = reinterpret_cast<u32*>(&store->records[count]);
    u32* first = table + mask + 1;
    u32* where = first + rows;
    clear_bytes(table, (mask + 1) * static_cast<u32>(sizeof(u32)));
    u64 lowest = ~u64{0}, highest = 0;
    for (u32 i = 0; i < rows; ++i) {
        const u64 user_id = row_field(packed, i, __builtin_offsetof(VoiceState, user_id));
        if (!user_id)
            continue;
        const u64 guild_id = row_field(packed, i, __builtin_offsetof(VoiceState, guild_id));
        lowest = minimum(lowest, guild_id);
        highest = maximum(highest, guild_id);
        const u32 slot = key_slot(table, mask, shift, packed, guild_id, user_id);
        if (!table[slot]) {
            table[slot] = i + 1;
            where[i] = ~0u;
        }
        first[i] = table[slot] - 1;
    }
    for (u32 i = 0; i < count; ++i) {
        const auto& record = store->records[i];
        if (record.guild_id - lowest > highest - lowest)
            continue;
        // The store holds one state for each key.
        if (const u32 entry = table[key_slot(table, mask, shift, packed, record.guild_id, record.user_id)])
            where[entry - 1] = i;
    }
    u32 joined = 0;
    bool changed = false;
    for (u32 i = 0; i < rows; ++i) {
        const VoiceState row = row_at(packed, i);
        if (!row.user_id)
            continue;
        u32& at = where[first[i]];
        VoiceState* record = at == ~0u ? nullptr : &store->records[at];
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
        at = row.channel_id ? count + rows + joined : ~0u;
        if (row.channel_id) {
            joins[joined++] = row;
            changed = true;
        }
    }
    const u32 kept = compact(store->records.data, store->count, &VoiceState::channel_id, 0);
    const u32 arrivals = compact(joins, joined, &VoiceState::channel_id, 0);
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

ConstSlice<VoiceState> voice_state_store_channel(const VoiceStateStore& store, u64 channel_id) noexcept {
    if (!store_valid(store) || !channel_id)
        return {};
    const ChannelRun run = channel_run(store, channel_id);
    return {store.records.data + run.first, run.count};
}

bool voice_state_store_set_speaking(VoiceStateStore* store, u64 channel_id,
                                            ConstSlice<u64> users) noexcept {
    if (!store || !store_valid(*store))
        return false;
    bool changed = false;
    const ChannelRun run = channel_id ? channel_run(*store, channel_id) : ChannelRun{store->count, 0};
    // One unsigned compare: before the run, i - run.first wraps past run.count.
    for (u32 i = 0; i < store->count; ++i) {
        auto& record = store->records[i];
        bool speaking = false;
        for (u32 k = 0; i - run.first < run.count && k < users.count && !speaking; ++k)
            speaking = users[k] == record.user_id;
        const u32 flags = speaking ? record.flags | voice_speaking : record.flags & ~u32{voice_speaking};
        changed = changed || flags != record.flags;
        record.flags = flags;
    }
    return changed;
}

} // namespace maicord
