#include "maicord/model/stage_instance_store.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
u32 index_of(const StageInstanceStore& store, u64 channel_id) noexcept {
    for (u32 i = 0; i < store.count; ++i)
        if (store.records[i].channel_id == channel_id)
            return i;
    return ~0u;
}
void remove_at(StageInstanceStore* store, u32 index) noexcept {
    for (u32 i = index + 1; i < store->count; ++i)
        copy_value(&store->records[i - 1], store->records[i]);
    reset_value(&store->records[--store->count]);
    ++store->revision;
}
} // namespace

void stage_instance_store_reset(StageInstanceStore* store) noexcept {
    if (!store)
        return;
    const bool changed = store->count != 0;
    for (u32 i = 0; i < store->count; ++i)
        reset_value(&store->records[i]);
    store->count = 0;
    store->revision += changed ? 1u : 0u;
}

bool stage_instance_store_apply(StageInstanceStore* store, u64 id, u64 guild_id, u64 channel_id, u8 privacy_level,
                                Utf8View topic) noexcept {
    if (!store || !channel_id || (topic.bytes && !topic.data) || store->count > kStageInstanceCapacity)
        return false;
    const u32 existing = index_of(*store, channel_id);
    if (!id) {
        if (existing != ~0u)
            remove_at(store, existing);
        return true;
    }
    if (!guild_id)
        return false;
    if (existing == ~0u && store->count == kStageInstanceCapacity) {
        ++store->dropped;
        return false;
    }
    u32 bytes = minimum(topic.bytes, kStageInstanceTopicBytes);
    while (bytes && bytes < topic.bytes && (static_cast<u8>(topic.data[bytes]) & 0xc0) == 0x80)
        --bytes;
    StageInstance next{};
    next.id = id;
    next.guild_id = guild_id;
    next.channel_id = channel_id;
    next.privacy_level = privacy_level;
    next.topic_bytes = static_cast<u16>(bytes);
    copy_bytes(next.topic, topic.data, bytes);
    auto& slot = store->records[existing == ~0u ? store->count++ : existing];
    if (existing != ~0u && slot.id == next.id && slot.guild_id == next.guild_id &&
        slot.privacy_level == next.privacy_level && slot.topic_bytes == next.topic_bytes &&
        bytes_equal(slot.topic, next.topic, bytes))
        return true;
    copy_value(&slot, next);
    ++store->revision;
    return true;
}

bool stage_instance_store_remove(StageInstanceStore* store, u64 channel_id) noexcept {
    const u32 index = store ? index_of(*store, channel_id) : ~0u;
    if (index == ~0u)
        return false;
    remove_at(store, index);
    return true;
}

bool stage_instance_store_clear_guild(StageInstanceStore* store, u64 guild_id) noexcept {
    if (!store)
        return false;
    bool changed = false;
    for (u32 i = store->count; i-- > 0;)
        if (store->records[i].guild_id == guild_id) {
            remove_at(store, i);
            changed = true;
        }
    return changed;
}

const StageInstance* stage_instance_store_find(const StageInstanceStore& store, u64 channel_id) noexcept {
    const u32 index = channel_id ? index_of(store, channel_id) : ~0u;
    return index == ~0u ? nullptr : &store.records[index];
}
} // namespace maicord
