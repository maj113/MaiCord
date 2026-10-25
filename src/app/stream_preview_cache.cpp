#include "maicord/app/stream_preview_cache.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
namespace {
StreamPreviewEntry* find(StreamPreviewCache& cache, const GatewayStreamKey& key) noexcept {
    for (auto& entry : cache.entries)
        if (entry.key.type != GatewayStreamType::none && entry.key == key)
            return &entry;
    return nullptr;
}
bool wanted(const StreamPreviewEntry& entry, u64 now_ms) noexcept {
    return entry.key.type != GatewayStreamType::none && entry.wanted_ms && now_ms - entry.wanted_ms < kStreamPreviewWantMs;
}
void erase(StreamPreviewEntry& entry) noexcept {
    // The URL is a bearer of unencrypted media: wipe it, not just its length.
    clear_bytes(&entry, sizeof(entry));
}
} // namespace

StreamPreviewEntry* stream_preview_want(StreamPreviewCache* cache, const GatewayStreamKey& key, u64 now_ms) noexcept {
    if (!cache || !gateway_stream_key_valid(key))
        return nullptr;
    auto* entry = find(*cache, key);
    if (!entry) {
        for (auto& candidate : cache->entries) {
            if (candidate.request_id)
                continue;
            if (candidate.key.type == GatewayStreamType::none) {
                entry = &candidate;
                break;
            }
            if (!entry || candidate.wanted_ms < entry->wanted_ms)
                entry = &candidate;
        }
        if (!entry)
            return nullptr;
        erase(*entry);
        entry->key = key;
    }
    entry->wanted_ms = now_ms ? now_ms : 1;
    return entry;
}

const StreamPreviewEntry* stream_preview_next_due(const StreamPreviewCache& cache, u64 now_ms) noexcept {
    const StreamPreviewEntry* best = nullptr;
    for (const auto& entry : cache.entries)
        if (wanted(entry, now_ms) && !entry.request_id && now_ms >= entry.next_ms &&
            (!best || entry.wanted_ms > best->wanted_ms))
            best = &entry;
    return best;
}

u64 stream_preview_next_wake(const StreamPreviewCache& cache, u64 now_ms) noexcept {
    u64 wake = 0;
    for (const auto& entry : cache.entries) {
        if (!wanted(entry, now_ms) || entry.request_id)
            continue;
        // The host re-wants what is still shown when this wake fires.
        const u64 due = entry.next_ms > now_ms ? entry.next_ms : now_ms;
        if (!wake || due < wake)
            wake = due;
    }
    return wake;
}

bool stream_preview_submitted(StreamPreviewCache* cache, const GatewayStreamKey& key, u64 request_id,
                              u64 now_ms) noexcept {
    auto* entry = cache && request_id ? find(*cache, key) : nullptr;
    if (!entry || entry->request_id)
        return false;
    entry->request_id = request_id;
    entry->next_ms = now_ms;
    return true;
}

void stream_preview_deferred(StreamPreviewCache* cache, const GatewayStreamKey& key, u64 retry_ms) noexcept {
    if (auto* entry = cache ? find(*cache, key) : nullptr; entry && !entry->request_id)
        entry->next_ms = retry_ms;
}

const StreamPreviewEntry* stream_preview_complete(StreamPreviewCache* cache, u64 request_id, u16 status,
                                                  Utf8View url, u64 retry_at_ms, u64 now_ms) noexcept {
    if (!cache || !request_id)
        return nullptr;
    StreamPreviewEntry* entry = nullptr;
    for (auto& candidate : cache->entries)
        if (candidate.request_id == request_id)
            entry = &candidate;
    if (!entry)
        return nullptr;
    entry->request_id = 0;
    entry->status = status;
    if (status == 200 && url.data && url.bytes && url.bytes <= sizeof(entry->url)) {
        // A new URL is a new picture; an unchanged one keeps its identity.
        const bool same = entry->serial && entry->url_bytes == url.bytes && bytes_equal(entry->url, url.data, url.bytes);
        if (!same) {
            clear_bytes(entry->url, sizeof(entry->url));
            copy_bytes(entry->url, url.data, url.bytes);
            entry->url_bytes = url.bytes;
            if (++cache->serial == 0)
                ++cache->serial;
            entry->serial = cache->serial;
        }
        entry->attempts = 0;
        entry->next_ms = now_ms + kStreamPreviewCacheMs;
        return entry;
    }
    if (entry->attempts < kStreamPreviewMaximumAttempts)
        ++entry->attempts;
    u64 next = now_ms + kStreamPreviewRetryMs * entry->attempts;
    if (status == 429 && retry_at_ms > next)
        next = retry_at_ms;
    entry->next_ms = next;
    return entry;
}

void stream_preview_clear(StreamPreviewCache* cache) noexcept {
    if (cache)
        clear_bytes(cache, sizeof(*cache));
}

MediaAssetKey stream_preview_media_key(const StreamPreviewEntry& entry) noexcept {
    if (!entry.serial || !entry.key.owner_id)
        return {};
    return media_asset_embed(entry.key.owner_id, entry.serial, 0, MediaEmbedResource::image,
                             MediaAttachmentFormat::jpeg);
}

bool stream_preview_ready(const StreamPreviewEntry& entry) noexcept {
    return entry.serial && entry.url_bytes;
}
} // namespace maicord
