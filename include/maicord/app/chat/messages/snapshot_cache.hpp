#pragma once

#include "maicord/app/chat/messages/history_sync.hpp"
#include "maicord/core/arena.hpp"

namespace maicord {

// Retained per-channel timeline windows for instant channel switching. Rows are
// packed back to back in one bounded pool (strings relocated, rich data packed
// by the rich store) and restored into the live timeline on return.
inline constexpr u32 kMessageSnapshotCacheEntries = 6;
inline constexpr u32 kMessageSnapshotCacheBudgetBytes = 1536 * 1024;
inline constexpr u32 kMessageSnapshotRichBankBytes = 64 * 1024;

struct MessageSnapshotRow {
    u64 id;
    u64 author_id;
    u64 timestamp_ms;
    u64 edited_timestamp_ms;
    u32 bytes; // whole row, 8-byte aligned: header, content, packed rich data
    u32 flags;
    u32 item_units;
    u32 content_bytes;
    u32 rich_bytes;
    u16 type;
    MessageContentAvailability content_availability;
    u8 presentation_flags;
};

static_assert(sizeof(MessageSnapshotRow) == 56);

struct MessageSnapshotEntry {
    u64 channel_id;
    u64 newest_id;
    u64 anchor_id;
    u64 older_cursor;
    u64 used_tick;
    u32 offset;
    u32 bytes;
    u32 count;
    u32 creates; // live creates observed while cached; bounds the newer-history gap
    u32 anchor_units;
    bool occupied;
    bool follow_bottom;
    bool live_edge; // the window reached the server tail when captured
    bool older_exhausted;
    bool speculative; // prefetched and never shown; evicted first
};

struct MessageSnapshotScratch;

struct MessageSnapshotCache {
    MessageSnapshotEntry entries[kMessageSnapshotCacheEntries];
    Arena pool;
    Arena scratch_arena;
    MessageSnapshotScratch* scratch;
    u64 tick;
    u32 used;
    u32 budget;
};

struct MessageSnapshotCaptureInput {
    u64 channel_id;
    u64 older_cursor;
    bool older_exhausted;
    bool live_edge;
    bool speculative;
};

enum class MessageSnapshotRestoreStatus : u8 {
    restored,
    missing,
    gap,
    failed,
};

struct MessageSnapshotRestoreResult {
    u64 older_cursor;
    u32 count;
    MessageSnapshotRestoreStatus status;
    bool older_exhausted;
    bool live_edge;
};

// Allocates nothing; the pool is reserved on the first capture.
void message_snapshot_cache_initialize(MessageSnapshotCache* cache, u32 budget_bytes) noexcept;
// Drops every snapshot and returns all memory.
void message_snapshot_cache_release(MessageSnapshotCache* cache) noexcept;
void message_snapshot_cache_drop(MessageSnapshotCache* cache, u64 channel_id) noexcept;
// Drops every snapshot whose channel the predicate rejects.
void message_snapshot_cache_prune(MessageSnapshotCache* cache, bool (*keep)(void*, u64 channel_id),
                                  void* context) noexcept;
const MessageSnapshotEntry* message_snapshot_cache_find(const MessageSnapshotCache& cache,
                                                        u64 channel_id) noexcept;
u32 message_snapshot_cache_count(const MessageSnapshotCache& cache) noexcept;

// Packs the authoritative rows of a single-channel timeline. Local outbox rows
// are omitted; their owner re-projects them after a restore.
bool message_snapshot_cache_capture(MessageSnapshotCache* cache, const MessageTimeline& timeline,
                                    const MessageSnapshotCaptureInput& input) noexcept;
// Builds a speculative snapshot from one decoded newest-first history page.
bool message_snapshot_cache_capture_page(MessageSnapshotCache* cache, u64 channel_id,
                                         ConstSlice<GatewayMessageEvent> messages,
                                         const ChatStateStore* state,
                                         MessageContentAvailability empty_content_availability,
                                         u32 default_item_units, bool exhausted) noexcept;
// Restores into an empty timeline and consumes the entry. A gap (at least
// gap_limit creates, or a newer channel tail without an observed create) or a
// failure drops the entry and leaves the timeline empty for the cold path.
MessageSnapshotRestoreResult message_snapshot_cache_restore(MessageSnapshotCache* cache, u64 channel_id,
                                                            u64 channel_last_message_id, u32 gap_limit,
                                                            MessageTimeline* timeline) noexcept;
// Follows a validated Gateway dispatch for a channel without a live timeline:
// creates widen the gap bound, deletes remove rows, updates and reactions are
// replayed through the canonical timeline/rich-store code on one row.
void message_snapshot_cache_observe(MessageSnapshotCache* cache, const GatewayClientEvent& event,
                                    ConstSlice<u8> payload, Slice<u64> bulk_ids, u64 self_user_id,
                                    MessageContentAvailability content_availability) noexcept;

// Press-intent preload policy: a pointer press on a channel row that waits for
// its release to open the channel requests the row's latest page, so the click
// finds it in flight or cached. One request in flight, per-channel
// deduplication and a short window for unused results. Hovering requests nothing.
inline constexpr u32 kChannelPrefetchRepeatMs = 15'000;
inline constexpr u32 kChannelPrefetchKeepMs = 2'000;
inline constexpr u32 kChannelPrefetchRecent = 4;

struct ChannelPrefetch {
    u64 press_channel_id;
    u64 press_at_ms;
    u64 left_at_ms;
    u64 inflight_channel_id;
    u64 inflight_request_id;
    u64 recent_channel_ids[kChannelPrefetchRecent];
    u64 recent_at_ms[kChannelPrefetchRecent];
    bool fired;
};

// The latest press wins; channel_id 0 is ignored.
void channel_prefetch_press(ChannelPrefetch* prefetch, u64 channel_id, u64 now_ms) noexcept;
u64 channel_prefetch_due(const ChannelPrefetch& prefetch, u64 now_ms) noexcept;
u64 channel_prefetch_next_wake(const ChannelPrefetch& prefetch) noexcept;
// Settles the current press without a request (already cached or unreadable).
void channel_prefetch_skip(ChannelPrefetch* prefetch) noexcept;
void channel_prefetch_submitted(ChannelPrefetch* prefetch, u64 channel_id, u64 request_id,
                                u64 now_ms) noexcept;
// The selected channel takes over the in-flight request as its history load.
bool channel_prefetch_adopt(ChannelPrefetch* prefetch, u64 request_id, u64 channel_id) noexcept;
// True when the finished request is still wanted (latest press or recently replaced).
bool channel_prefetch_complete(ChannelPrefetch* prefetch, u64 request_id, u64 now_ms) noexcept;

} // namespace maicord
