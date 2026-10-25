#include "maicord/app/media_url_resolver.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/deadline.hpp"
#include "rest_rate_retry.hpp"

namespace maicord {
MediaUrlState media_url_resolution_state(const MediaUrlResolver& resolver, Utf8View source) noexcept {
    if (!source.data || !source.bytes || source.bytes > kCdnMaximumTargetBytes)
        return MediaUrlState::empty;
    for (const auto& entry : resolver.entries) {
        if (entry.state != MediaUrlState::empty && entry.original_bytes == source.bytes &&
            bytes_equal(entry.original, source.data, source.bytes))
            return entry.state;
    }
    return MediaUrlState::empty;
}
Utf8View media_url_resolve(MediaUrlResolver* resolver, Utf8View source, u64 now_ms,
                           u64 now_seconds, bool force_refresh) noexcept {
    if (force_refresh) {
        // Validate the same strict attachment rules used by the POST
        // encoder before accepting a far-future source into this cache.
        if (!resolver || !source.data || !source.bytes || source.bytes > kCdnMaximumTargetBytes) return {};
        u8 admission[kCdnMaximumTargetBytes + 64];
        if (!attachment_refresh_body(source, {admission, sizeof(admission)})) return {};
    } else if (!resolver || !attachment_refresh_needed(source, now_seconds))
        return source;
    if (source.bytes > kCdnMaximumTargetBytes)
        return {};
    MediaUrlEntry* replace = nullptr;
    for (auto& entry : resolver->entries) {
        if (entry.state != MediaUrlState::empty && entry.original_bytes == source.bytes &&
            bytes_equal(entry.original, source.data, source.bytes)) {
            entry.last_used_ms = now_ms;
            if (entry.state == MediaUrlState::ready) {
                const Utf8View refreshed{entry.refreshed, entry.refreshed_bytes};
                if (!attachment_refresh_needed(refreshed, now_seconds))
                    return refreshed;
                // Only a new begin call may rearm a forced result.
                // An expired ready result fails that stored intent.
                if (force_refresh) return {};
                entry.state = MediaUrlState::queued;
                entry.attempts = 0;
            }
            return {};
        }
        if (entry.state == MediaUrlState::empty || entry.state == MediaUrlState::ready ||
            (entry.state == MediaUrlState::failed && now_ms - entry.last_used_ms > 60'000)) {
            if (!replace || entry.last_used_ms < replace->last_used_ms)
                replace = &entry;
        }
    }
    if (!replace)
        return {};
    reset_value<MediaUrlEntry>(replace);
    copy_bytes(replace->original, source.data, source.bytes);
    replace->original_bytes = static_cast<u16>(source.bytes);
    replace->last_used_ms = now_ms;
    replace->state = MediaUrlState::queued;
    return {};
}

MediaUrlEntry* media_url_next(MediaUrlResolver* resolver, u64 now_ms) noexcept {
    if (!resolver || resolver->pending_slot != 0)
        return nullptr;
    for (auto& entry : resolver->entries) {
        if (entry.state == MediaUrlState::queued && entry.retry_at_ms <= now_ms)
            return &entry;
    }
    return nullptr;
}

void media_url_complete(MediaUrlResolver* resolver, const RestClientResponse& response,
                        Utf8View body, u64 now_ms, u64 now_seconds) noexcept {
    if (!resolver || resolver->pending_slot == 0 || resolver->pending_slot > 32)
        return;
    auto& entry = resolver->entries[resolver->pending_slot - 1];
    resolver->pending_slot = 0;
    entry.retry_at_ms = 0;
    if (response.transport_ok() && response.status_code == 200) {
        entry.refreshed_bytes = static_cast<u16>(
            attachment_refresh_response(body, {entry.original, entry.original_bytes}, now_seconds,
                                        {entry.refreshed, sizeof(entry.refreshed)}));
        if (entry.refreshed_bytes != 0) {
            entry.state = MediaUrlState::ready;
            return;
        }
    }
    const bool limited = response.status_code == 429;
    const u64 rate_deadline = app_detail::rest_rate_retry_deadline(response);
    const bool transient =
        limited ? rate_deadline != 0 : !response.transport_ok() || response.status_code >= 500;
    if (transient && entry.attempts < 3) {
        entry.state = MediaUrlState::queued;
        entry.retry_at_ms =
            limited ? rate_deadline : maximum(response.retry_at_ms, now_ms + (1000ull << entry.attempts));
    } else {
        entry.state = MediaUrlState::failed;
    }
}

u64 media_url_next_wake(const MediaUrlResolver& resolver) noexcept {
    u64 wake = 0;
    for (const auto& entry : resolver.entries) {
        if (entry.state == MediaUrlState::queued)
            wake = earliest_deadline(wake, entry.retry_at_ms);
    }
    return wake;
}
} // namespace maicord
