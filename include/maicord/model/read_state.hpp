#pragma once
#include "maicord/model/chat_state.hpp"

namespace maicord {
enum ReadStateField : u8 {
    read_state_cursor = 1u << 0,
    read_state_mentions = 1u << 1,
    read_state_last_viewed = 1u << 2,
    read_state_flags = 1u << 3,
    read_state_manual = 1u << 4,
    read_state_mentions_incomplete = 1u << 5, // internal cached estimate
};
// Only unmatched channel IDs reside here. The corresponding known channel
// record holds its cursor, count and metadata without a second indexed cache.
struct ReadStateEntry {
    u64 channel_id;
    u64 message_id;
    u32 mention_count;
    u32 last_viewed;
    u32 flags;
    u8 fields;
    u8 reserved[3];
};
static_assert(sizeof(ReadStateEntry) == 32);
using ReadStateReserve = bool (*)(void* context, u32 required_count) noexcept;
struct ReadStateStore {
    Slice<ReadStateEntry> pending;
    ReadStateReserve reserve;
    void* reserve_context;
    u64 version;
    u32 count;
    u32 backed_count;
    u32 revision;
    u32 generation;
    u32 rejected;
    bool incomplete;
    bool preparing_ready;
    bool retaining_ready;
};
bool read_state_initialize(ReadStateStore* reads,
    Slice<ReadStateEntry> pending) noexcept;
// Slice count is reserved capacity. A callback must back the requested prefix
// before returning true. Without one, callers provide fully writable storage.
bool read_state_set_reserve(ReadStateStore* reads,
    ReadStateReserve reserve, void* context) noexcept;
bool read_state_bind(ChatStateStore* state, ReadStateStore* reads) noexcept;
bool read_state_reserve_pending(ReadStateStore* reads, u32 required_count) noexcept;
// Call after whole-packet validation and before READY clears channels. Partial
// snapshots carry omitted loaded entries into unmatched storage on removal.
bool read_state_prepare_ready(ChatStateStore* state, bool partial,
    u64 version) noexcept;
void read_state_finish_ready(ChatStateStore* state) noexcept;
void read_state_clear(ReadStateStore* reads) noexcept;
bool read_state_upsert(ChatStateStore* state,
    const ReadStateEntry& entry, u64 version) noexcept;
// Counts reconstructed from partial history remain estimates when
// mentions_known is false; their presence never implies the history is fully loaded.
bool read_state_apply_ack(ChatStateStore* state, u64 channel_id,
    u64 message_id, bool manual, u32 mention_count, bool mentions_known,
    u64 version) noexcept;
bool read_state_mark_local(ChatStateStore* state, u64 channel_id,
    u64 message_id, u32 last_viewed, u32 flags) noexcept;
// Queue the manual ACK successfully before this optimistic mutation. cursor
// is the captured predecessor boundary (zero is a valid beginning boundary).
// A cached mention estimate remains incomplete; full u32 metadata
// and snapshot version are preserved. Selected automatic ACK stays suppressed
// until leave_channel or a user mark-read action.
bool read_state_mark_unread_local(ChatStateStore* state, u64 channel_id,
    u64 cursor, u32 mention_count, bool mentions_known) noexcept;
void read_state_leave_channel(ChatStateStore* state, u64 channel_id) noexcept;
const ReadStateEntry* read_state_pending(const ReadStateStore& reads,
    u64 channel_id) noexcept;
// State insertion/removal hooks preserve aggregate accounting in the store.
bool read_state_apply_pending(ChatStateStore* state, ChannelRecord* channel) noexcept;
bool read_state_remove_channel(ChatStateStore* state, const ChannelRecord& channel) noexcept;
// Central known-channel mutation, also used by local ACK planning.
bool chat_state_store_apply_read_state(ChatStateStore* state,
    const ReadStateEntry& entry) noexcept;
bool read_state_assign_record(ChannelRecord* channel,
    const ReadStateEntry& entry, bool selected) noexcept;
} // namespace maicord
