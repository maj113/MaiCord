#pragma once
#include "maicord/protocol/attachment_refresh.hpp"

namespace maicord {
struct RestClientResponse;
enum class MediaUrlState : u8 {
    empty,
    queued,
    pending,
    ready,
    failed
};
struct MediaUrlEntry {
    char original[kCdnMaximumTargetBytes];
    char refreshed[kCdnMaximumTargetBytes];
    u64 last_used_ms;
    u64 retry_at_ms;
    u16 original_bytes;
    u16 refreshed_bytes;
    u8 attempts;
    MediaUrlState state;
};
struct MediaUrlResolver {
    MediaUrlEntry entries[32];
    u32 pending_slot; // slot + 1; one request on the shared REST scheduler
};
// An empty result means pending/failed, never permission to fetch an expired URL.
// Forced resolution follows a re-armed attachment refresh, even when the
// original URL has a distant expiry. It does not re-arm failures or ready
// results itself; ordinary callers keep the original fast path.
Utf8View media_url_resolve(MediaUrlResolver* resolver, Utf8View source, u64 now_ms,
                           u64 now_seconds, bool force_refresh = false) noexcept;
// Read the source entry without changing retry or eviction state.
// Empty means there is no matching entry, not a permanent refresh failure.
MediaUrlState media_url_resolution_state(const MediaUrlResolver&, Utf8View source) noexcept;
MediaUrlEntry* media_url_next(MediaUrlResolver* resolver, u64 now_ms) noexcept;
void media_url_complete(MediaUrlResolver* resolver, const RestClientResponse& response,
                        Utf8View body, u64 now_ms, u64 now_seconds) noexcept;
u64 media_url_next_wake(const MediaUrlResolver& resolver) noexcept;
} // namespace maicord
