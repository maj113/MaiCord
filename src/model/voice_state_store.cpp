#include "maicord/model/voice_state_store.hpp"

namespace maicord {
namespace {
bool store_valid(const VoiceStateStore& store) noexcept {
    return store.records.data && store.records.count && store.records.count <= kVoiceStateCapacity &&
           store.count <= store.records.count;
}
u32 find_index(const VoiceStateStore& store, u64 guild_id, u64 user_id) noexcept {
    for (u32 i = 0; i < store.count; ++i)
        if (store.records[i].guild_id == guild_id && store.records[i].user_id == user_id)
            return i;
    return ~0u;
}
void remove_at(VoiceStateStore* store, u32 index) noexcept {
    for (u32 i = index + 1; i < store->count; ++i)
        store->records[i - 1] = store->records[i];
    store->records[--store->count] = {};
}
bool initialize_store(VoiceStateStore* store, Slice<VoiceState> records, bool sparse) noexcept {
    if (!store || !records.data || !records.count || records.count > kVoiceStateCapacity ||
        (sparse && store->count))
        return false;
    *store = {};
    store->records = records;
    if (sparse) return true;
    for (u32 i = 0; i < records.count; ++i)
        records[i] = {};
    return true;
}
} // namespace

bool voice_state_store_initialize(VoiceStateStore* store, Slice<VoiceState> records) noexcept {
    return initialize_store(store, records, false);
}

bool voice_state_store_initialize_sparse(VoiceStateStore* store, Slice<VoiceState> records) noexcept {
    return initialize_store(store, records, true);
}

bool voice_state_store_reset(VoiceStateStore* store) noexcept {
    if (!store || !store_valid(*store))
        return false;
    const bool changed = store->count != 0;
    for (u32 i = 0; i < store->count; ++i)
        store->records[i] = {};
    store->count = 0;
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
        remove_at(store, existing);
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
        remove_at(store, existing);
    } else if (store->count == store->records.count) {
        saturating_increment(store->dropped);
        return VoiceStateResult::capacity;
    }
    store->records[store->count++] = {guild_id, channel_id, user_id, flags & ~u32{voice_speaking},
                                      request_to_speak_s};
    saturating_increment(store->revision);
    return VoiceStateResult::changed;
}

VoiceStateResult voice_state_store_clear_guild(VoiceStateStore* store,
                                                              u64 guild_id) noexcept {
    if (!store || !store_valid(*store))
        return VoiceStateResult::invalid_input;
    u32 kept = 0;
    for (u32 i = 0; i < store->count; ++i)
        if (store->records[i].guild_id != guild_id)
            store->records[kept++] = store->records[i];
    if (kept == store->count)
        return VoiceStateResult::unchanged;
    for (u32 i = kept; i < store->count; ++i)
        store->records[i] = {};
    store->count = kept;
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
    u32 count = 0;
    if (store_valid(store) && channel_id)
        for (u32 i = 0; i < store.count; ++i)
            count += store.records[i].channel_id == channel_id;
    return count;
}

u32 voice_state_store_channel_live(const VoiceStateStore& store, u64 channel_id, Slice<u64> users) noexcept {
    u32 count = 0;
    if (!store_valid(store) || !channel_id || (!users.data && users.count))
        return 0;
    for (u32 i = 0; i < store.count; ++i) {
        const auto& state = store.records[i];
        if (state.channel_id != channel_id || !voice_state_live(state))
            continue;
        if (count < users.count)
            users.data[count] = state.user_id;
        ++count;
    }
    return count;
}

const VoiceState* voice_state_store_channel_at(const VoiceStateStore& store,
                                                              u64 channel_id, u32 index) noexcept {
    if (!store_valid(store) || !channel_id)
        return nullptr;
    for (u32 i = 0; i < store.count; ++i)
        if (store.records[i].channel_id == channel_id && index-- == 0)
            return &store.records[i];
    return nullptr;
}

bool voice_state_store_set_speaking(VoiceStateStore* store, u64 channel_id,
                                            ConstSlice<u64> users) noexcept {
    if (!store || !store_valid(*store))
        return false;
    bool changed = false;
    for (u32 i = 0; i < store->count; ++i) {
        auto& record = store->records[i];
        bool speaking = false;
        for (u32 k = 0; channel_id && record.channel_id == channel_id && k < users.count && !speaking; ++k)
            speaking = users[k] == record.user_id;
        const u32 flags = speaking ? record.flags | voice_speaking : record.flags & ~u32{voice_speaking};
        changed = changed || flags != record.flags;
        record.flags = flags;
    }
    saturating_increment_if(changed, store->revision);
    return changed;
}

} // namespace maicord
