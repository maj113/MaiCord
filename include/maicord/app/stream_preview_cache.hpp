#pragma once
#include "maicord/media/asset_key.hpp"
#include "maicord/protocol/gateway_stream.hpp"

namespace maicord {
// Go Live preview thumbnails (GET /streams/{key}/preview), keyed by stream.
// Like the official client: a fetched preview is reused for 120 s, a failed
// fetch (404 before the first thumbnail, transport errors) backs off
// 10 s x attempts, and a 429 waits at least retry_after. Only streams a
// surface wants right now (hover card, visible call tile) are fetched; the
// last good image stays shown while a refresh is in flight or backing off.
// Preview URLs are not end-to-end encrypted: never log them.
inline constexpr u32 kStreamPreviewEntries = 8;
inline constexpr u64 kStreamPreviewCacheMs = 120'000;
inline constexpr u64 kStreamPreviewRetryMs = 10'000;
inline constexpr u32 kStreamPreviewMaximumAttempts = 6; // Backoff stops growing at 60 s.
// A want older than this no longer schedules fetches (the surface closed).
inline constexpr u64 kStreamPreviewWantMs = 1'500;

struct StreamPreviewEntry {
    GatewayStreamKey key;
    u64 request_id;  // Nonzero while a fetch is in flight.
    u64 next_ms;     // Earliest next fetch (expiry or backoff).
    u64 wanted_ms;   // Last time a surface showed this stream.
    u32 serial;      // Fetch serial of the current URL (media identity); zero: none yet.
    u32 url_bytes;
    u16 attempts;    // Consecutive failures.
    u16 status;      // Last HTTP status (0: transport failure or never fetched).
    char url[kGatewayStreamPreviewMaximumUrlBytes];
};
struct StreamPreviewCache {
    StreamPreviewEntry entries[kStreamPreviewEntries];
    u32 serial;
};

// The entry for key (created, or recycling the least recently wanted idle
// entry), stamped as wanted at now_ms. Null for an invalid key or when every
// entry has a fetch in flight.
StreamPreviewEntry* stream_preview_want(StreamPreviewCache* cache, const GatewayStreamKey& key, u64 now_ms) noexcept;
// The most recently wanted entry whose fetch is due (wanted, idle, past next_ms).
const StreamPreviewEntry* stream_preview_next_due(const StreamPreviewCache& cache, u64 now_ms) noexcept;
// Earliest future time a wanted entry becomes due; zero when nothing waits.
u64 stream_preview_next_wake(const StreamPreviewCache& cache, u64 now_ms) noexcept;
bool stream_preview_submitted(StreamPreviewCache* cache, const GatewayStreamKey& key, u64 request_id,
                              u64 now_ms) noexcept;
// A submit refused for now (lane busy): retry after delay_ms without counting a failure.
void stream_preview_deferred(StreamPreviewCache* cache, const GatewayStreamKey& key, u64 retry_ms) noexcept;
// Completes the fetch request_id. url is the decoded preview URL (empty on
// any failure); retry_at_ms is the response's 429 deadline (zero if none).
// Returns the entry, or null when no fetch had that ID (cancelled/recycled).
const StreamPreviewEntry* stream_preview_complete(StreamPreviewCache* cache, u64 request_id, u16 status,
                                                  Utf8View url, u64 retry_at_ms, u64 now_ms) noexcept;
// Drops everything (sign-out).
void stream_preview_clear(StreamPreviewCache* cache) noexcept;
// Image identity of the entry's current URL: an embed-style key that changes
// with every successful fetch, so a refreshed thumbnail is fetched anew.
MediaAssetKey stream_preview_media_key(const StreamPreviewEntry& entry) noexcept;
bool stream_preview_ready(const StreamPreviewEntry& entry) noexcept;
} // namespace maicord
