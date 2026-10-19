#include "maicord/model/voice_channel_info_store.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
// Records stay sorted by channel ID.
u32 lower_bound(const VoiceChannelInfoStore& s, u64 channel_id) noexcept {
    u32 low = 0, high = s.count;
    while (low < high) {
        const u32 middle = (low + high) / 2;
        if (s.records[middle].channel_id < channel_id)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}
void drop_text(VoiceChannelInfoStore* s, VoiceChannelInfo* info) noexcept {
    if (!info->status_bytes)
        return;
    const u32 offset = info->status_offset, bytes = info->status_bytes;
    copy_bytes(s->text + offset, s->text + offset + bytes, s->text_used - offset - bytes);
    s->text_used -= bytes;
    for (u32 i = 0; i < s->count; ++i)
        if (s->records[i].status_bytes && s->records[i].status_offset > offset)
            s->records[i].status_offset -= bytes;
    info->status_offset = info->status_bytes = 0;
}
void erase(VoiceChannelInfoStore* s, u32 index) noexcept {
    drop_text(s, &s->records[index]);
    copy_bytes(&s->records[index], &s->records[index + 1],
               (s->count - index - 1) * sizeof(VoiceChannelInfo));
    --s->count;
}
// Frees one record or status of another guild; false when none exists.
bool evict(VoiceChannelInfoStore* s, u64 guild_id, bool text) noexcept {
    for (u32 i = 0; i < s->count; ++i)
        if (s->records[i].guild_id != guild_id && (!text || s->records[i].status_bytes)) {
            erase(s, i);
            ++s->dropped;
            return true;
        }
    return false;
}
// The channel's record, created when absent; null when full.
VoiceChannelInfo* slot(VoiceChannelInfoStore* s, u64 guild_id, u64 channel_id) noexcept {
    u32 index = lower_bound(*s, channel_id);
    if (index < s->count && s->records[index].channel_id == channel_id) {
        s->records[index].guild_id = guild_id;
        return &s->records[index];
    }
    if (s->count == kVoiceChannelInfoCapacity) {
        if (!evict(s, guild_id, false))
            return nullptr;
        index = lower_bound(*s, channel_id);
    }
    copy_bytes(&s->records[index + 1], &s->records[index], (s->count - index) * sizeof(VoiceChannelInfo));
    s->records[index] = {channel_id, guild_id};
    ++s->count;
    return &s->records[index];
}
bool settle(VoiceChannelInfoStore* s, u64 channel_id) noexcept {
    const u32 index = lower_bound(*s, channel_id);
    if (index < s->count && s->records[index].channel_id == channel_id && !s->records[index].start_s &&
        !s->records[index].status_bytes)
        erase(s, index);
    ++s->revision;
    return true;
}
bool valid(const VoiceChannelInfoStore* s, u64 guild_id, u64 channel_id) noexcept {
    return s && guild_id && channel_id;
}
} // namespace

void voice_channel_info_store_reset(VoiceChannelInfoStore* s) noexcept {
    if (!s)
        return;
    const u32 revision = s->revision + 1;
    s->count = s->text_used = s->dropped = 0;
    s->revision = revision;
}

bool voice_channel_info_store_set_start(VoiceChannelInfoStore* s, u64 guild_id, u64 channel_id,
                                        u64 start_s) noexcept {
    if (!valid(s, guild_id, channel_id) || start_s > ~0u)
        return false;
    const auto* current = voice_channel_info_store_find(*s, channel_id);
    if (current ? current->start_s == start_s && current->guild_id == guild_id : !start_s)
        return true;
    auto* info = slot(s, guild_id, channel_id);
    if (!info) {
        ++s->dropped;
        return false;
    }
    info->start_s = static_cast<u32>(start_s);
    return settle(s, channel_id);
}

bool voice_channel_info_store_set_status(VoiceChannelInfoStore* s, u64 guild_id, u64 channel_id,
                                         Utf8View status) noexcept {
    if (!valid(s, guild_id, channel_id) || (!status.data && status.bytes) || status.bytes > 0xffff ||
        status.bytes > kVoiceChannelInfoTextBytes)
        return false;
    if (const auto* current = voice_channel_info_store_find(*s, channel_id)) {
        if (current->guild_id == guild_id && utf8_equal(voice_channel_info_status(*s, *current), status))
            return true;
    } else if (!status.bytes) {
        return true;
    }
    auto* info = slot(s, guild_id, channel_id);
    if (!info) {
        ++s->dropped;
        return false;
    }
    drop_text(s, info);
    while (s->text_used + status.bytes > kVoiceChannelInfoTextBytes) {
        if (!evict(s, guild_id, true)) {
            ++s->dropped;
            settle(s, channel_id);
            return false;
        }
        info = const_cast<VoiceChannelInfo*>(voice_channel_info_store_find(*s, channel_id));
    }
    if (status.bytes) {
        copy_bytes(s->text + s->text_used, status.data, status.bytes);
        info->status_offset = s->text_used;
        info->status_bytes = static_cast<u16>(status.bytes);
        s->text_used += status.bytes;
    }
    return settle(s, channel_id);
}

bool voice_channel_info_store_clear_guild(VoiceChannelInfoStore* s, u64 guild_id) noexcept {
    if (!s || !guild_id)
        return false;
    bool changed = false;
    for (u32 i = s->count; i-- > 0;)
        if (s->records[i].guild_id == guild_id) {
            erase(s, i);
            changed = true;
        }
    s->revision += changed;
    return changed;
}

bool voice_channel_info_store_remove(VoiceChannelInfoStore* s, u64 channel_id) noexcept {
    if (!s || !channel_id)
        return false;
    const u32 index = lower_bound(*s, channel_id);
    if (index >= s->count || s->records[index].channel_id != channel_id)
        return false;
    erase(s, index);
    ++s->revision;
    return true;
}

const VoiceChannelInfo* voice_channel_info_store_find(const VoiceChannelInfoStore& s,
                                                      u64 channel_id) noexcept {
    const u32 index = lower_bound(s, channel_id);
    return index < s.count && s.records[index].channel_id == channel_id ? &s.records[index] : nullptr;
}
} // namespace maicord
