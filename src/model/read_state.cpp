#include "maicord/model/read_state.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool valid_entry(const ReadStateEntry& entry) noexcept {
    constexpr u8 fields = read_state_cursor | read_state_mentions | read_state_last_viewed |
        read_state_flags | read_state_manual | read_state_mentions_incomplete;
    return entry.channel_id && (entry.fields & read_state_cursor) && !(entry.fields & ~fields) &&
        !entry.reserved[0] && !entry.reserved[1] && !entry.reserved[2];
}
u32 lower_bound(const ReadStateStore& reads, u64 channel_id) noexcept {
    u32 first = 0, count = reads.count;
    while (count) {
        const u32 step = count / 2, current = first + step;
        if (reads.pending[current].channel_id < channel_id) { first = current + 1; count -= step + 1; }
        else count = step;
    }
    return first;
}
void erase(ReadStateStore* reads, u32 position) noexcept {
    for (u32 i = position + 1; i < reads->count; ++i) reads->pending[i - 1] = reads->pending[i];
    --reads->count;
    saturating_increment(reads->revision);
}
void next_generation(ReadStateStore* reads) noexcept {
    ++reads->generation;
    if (!reads->generation) ++reads->generation;
}
bool stage(ReadStateStore* reads, const ReadStateEntry& entry) noexcept {
    const u32 position = lower_bound(*reads, entry.channel_id);
    if (position < reads->count && reads->pending[position].channel_id == entry.channel_id) {
        auto merged = entry;
        const auto& old = reads->pending[position];
        if (!(entry.fields & read_state_flags) && (old.fields & read_state_flags)) {
            merged.flags = old.flags; merged.fields |= read_state_flags;
        }
        if (!(entry.fields & read_state_last_viewed) && (old.fields & read_state_last_viewed)) {
            merged.last_viewed = old.last_viewed; merged.fields |= read_state_last_viewed;
        }
        reads->pending[position] = merged;
        saturating_increment(reads->revision);
        return true;
    }
    if (!read_state_reserve_pending(reads, reads->count + 1)) return false;
    for (u32 i = reads->count; i > position; --i) reads->pending[i] = reads->pending[i - 1];
    reads->pending[position] = entry; ++reads->count;
    saturating_increment(reads->revision);
    return true;
}
}

bool read_state_initialize(ReadStateStore* reads, Slice<ReadStateEntry> pending) noexcept {
    if (!reads || !pending.data || !pending.count || pending.count > 65'535) return false;
    reset_value(reads); reads->pending = pending; reads->backed_count = pending.count; return true;
}
bool read_state_set_reserve(ReadStateStore* reads, ReadStateReserve reserve,
    void* context) noexcept {
    if (!reads || !reads->pending.data || reads->count || reads->preparing_ready) return false;
    reads->reserve = reserve; reads->reserve_context = context;
    reads->backed_count = reserve ? 0 : reads->pending.count;
    return true;
}
bool read_state_bind(ChatStateStore* state, ReadStateStore* reads) noexcept {
    if (!state || (reads && (!reads->pending.data || reads->pending.count < state->channel_capacity ||
        reads->count > reads->backed_count || reads->backed_count > reads->pending.count))) return false;
    state->read_state = reads;
    return true;
}
bool read_state_reserve_pending(ReadStateStore* reads, u32 required_count) noexcept {
    if (!reads || !reads->pending.data) return false;
    if (required_count > reads->pending.count ||
        (required_count > reads->backed_count && (!reads->reserve ||
            !reads->reserve(reads->reserve_context, required_count)))) {
        reads->incomplete = true; saturating_increment(reads->rejected); return false;
    }
    reads->backed_count = maximum(reads->backed_count, required_count);
    return true;
}
void read_state_clear(ReadStateStore* reads) noexcept {
    if (!reads) return;
    reads->count = 0; reads->version = 0; reads->incomplete = false;
    reads->preparing_ready = reads->retaining_ready = false;
    saturating_increment(reads->revision); next_generation(reads);
}
bool read_state_prepare_ready(ChatStateStore* state, bool partial, u64 version) noexcept {
    if (!state || !state->read_state) return false;
    auto& reads = *state->read_state;
    if (reads.preparing_ready) return false;
    if (partial) {
        u32 required = reads.count;
        for (u32 i = 0; i < state->channel_count; ++i) {
            const auto& channel = state->channels[state->channel_order[i]];
            if (channel.read_state_known && !read_state_pending(reads, channel.id)) ++required;
        }
        if (!read_state_reserve_pending(&reads, required)) return false;
    } else read_state_clear(&reads);
    reads.version = version; reads.preparing_ready = true; reads.retaining_ready = partial;
    if (partial) { saturating_increment(reads.revision); next_generation(&reads); }
    return true;
}
void read_state_finish_ready(ChatStateStore* state) noexcept {
    if (state && state->read_state) {
        state->read_state->preparing_ready = false;
        state->read_state->retaining_ready = false;
    }
}
const ReadStateEntry* read_state_pending(const ReadStateStore& reads, u64 channel_id) noexcept {
    if (!reads.pending.data || reads.count > reads.backed_count || !channel_id) return nullptr;
    const u32 position = lower_bound(reads, channel_id);
    return position < reads.count && reads.pending[position].channel_id == channel_id ? &reads.pending[position] : nullptr;
}
bool read_state_assign_record(ChannelRecord* channel, const ReadStateEntry& entry,
    bool selected) noexcept {
    if (!channel || channel->id != entry.channel_id || !valid_entry(entry)) return false;
    channel->last_read_message_id = entry.message_id;
    channel->mention_count = entry.fields & read_state_mentions ? entry.mention_count : 0;
    channel->read_state_known = true;
    channel->read_mentions_incomplete = (entry.fields & read_state_mentions_incomplete) != 0 ||
        ((entry.fields & read_state_manual) && !(entry.fields & read_state_mentions));
    if (entry.fields & read_state_last_viewed) {
        channel->read_last_viewed = entry.last_viewed; channel->read_last_viewed_known = true;
    }
    if (entry.fields & read_state_flags) { channel->read_flags = entry.flags; channel->read_flags_known = true; }
    if ((entry.fields & read_state_manual) && selected) channel->read_auto_ack_suppressed = true;
    return true;
}
bool read_state_upsert(ChatStateStore* state, const ReadStateEntry& entry, u64 version) noexcept {
    if (!state || !state->read_state || !valid_entry(entry)) return false;
    auto& reads = *state->read_state;
    if (chat_state_store_find_channel(*state, entry.channel_id)) {
        if (!chat_state_store_apply_read_state(state, entry)) return false;
        const u32 position = lower_bound(reads, entry.channel_id);
        if (position < reads.count && reads.pending[position].channel_id == entry.channel_id) erase(&reads, position);
    } else if (!stage(&reads, entry)) return false;
    // Versions are bookkeeping only: older cursor/version values are not rejected,
    // and a manual backward ACK restores unread state.
    reads.version = version;
    saturating_increment(reads.revision);
    return true;
}
bool read_state_apply_ack(ChatStateStore* state, u64 channel_id, u64 message_id,
    bool manual, u32 mention_count, bool mentions_known, u64 version) noexcept {
    ReadStateEntry entry{};
    entry.channel_id = channel_id; entry.message_id = message_id; entry.mention_count = mention_count;
    entry.fields = static_cast<u8>(read_state_cursor | read_state_mentions | (manual ? read_state_manual : 0) |
        (!mentions_known ? read_state_mentions_incomplete : 0));
    return read_state_upsert(state, entry, version);
}
bool read_state_mark_local(ChatStateStore* state, u64 channel_id, u64 message_id,
    u32 last_viewed, u32 flags) noexcept {
    if (!state || !chat_state_store_find_channel(*state, channel_id)) return false;
    ReadStateEntry entry{};
    entry.channel_id = channel_id; entry.message_id = message_id; entry.last_viewed = last_viewed; entry.flags = flags;
    entry.fields = read_state_cursor | read_state_mentions | read_state_last_viewed | read_state_flags;
    if (!chat_state_store_apply_read_state(state, entry)) return false;
    read_state_leave_channel(state, channel_id);
    return true;
}
void read_state_leave_channel(ChatStateStore* state, u64 channel_id) noexcept {
    if (!state) return;
    const auto* found = chat_state_store_find_channel(*state, channel_id);
    if (found) state->channels[found - state->channels].read_auto_ack_suppressed = false;
}
bool read_state_mark_unread_local(ChatStateStore* state, u64 channel_id,
    u64 cursor, u32 mention_count, bool mentions_known) noexcept {
    if (!state || !chat_state_store_find_channel(*state, channel_id)) return false;
    ReadStateEntry entry{};
    entry.channel_id = channel_id; entry.message_id = cursor; entry.mention_count = mention_count;
    entry.fields = static_cast<u8>(read_state_cursor | read_state_mentions | read_state_manual |
        (!mentions_known ? read_state_mentions_incomplete : 0));
    return chat_state_store_apply_read_state(state, entry);
}
bool read_state_apply_pending(ChatStateStore* state, ChannelRecord* channel) noexcept {
    if (!state || !channel) return false;
    if (!state->read_state) return true;
    auto& reads = *state->read_state;
    const u32 position = lower_bound(reads, channel->id);
    if (position == reads.count || reads.pending[position].channel_id != channel->id) return true;
    if (!read_state_assign_record(channel, reads.pending[position], state->selected_channel_id == channel->id)) return false;
    erase(&reads, position); return true;
}
bool read_state_remove_channel(ChatStateStore* state, const ChannelRecord& channel) noexcept {
    if (!state || !state->read_state) return true;
    auto& reads = *state->read_state;
    if (reads.preparing_ready && reads.retaining_ready && channel.read_state_known) {
        ReadStateEntry entry{};
        entry.channel_id = channel.id; entry.message_id = channel.last_read_message_id; entry.mention_count = channel.mention_count;
        entry.last_viewed = channel.read_last_viewed; entry.flags = channel.read_flags;
        entry.fields = static_cast<u8>(read_state_cursor | read_state_mentions |
            (channel.read_flags_known ? read_state_flags : 0) |
            (channel.read_last_viewed_known ? read_state_last_viewed : 0) |
            (channel.read_mentions_incomplete ? read_state_mentions_incomplete : 0));
        return stage(&reads, entry);
    }
    const u32 position = lower_bound(reads, channel.id);
    if (position < reads.count && reads.pending[position].channel_id == channel.id) erase(&reads, position);
    return true;
}
} // namespace maicord
