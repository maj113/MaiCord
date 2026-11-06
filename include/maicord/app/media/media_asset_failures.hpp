#pragma once

#include "maicord/media/core/asset_key.hpp"

namespace maicord {

// Assets whose decoded frame the renderer refused (validation, upload or the
// timeline of that one asset). A refusal belongs to the asset, never to the
// session: the asset waits out a doubling backoff instead of being fetched,
// decoded and refused again on every media pass, and is logged once.
inline constexpr u32 kMediaAssetFailureCapacity = 32;
inline constexpr u64 kMediaAssetFailureFirstBackoffMs = 30'000;
inline constexpr u64 kMediaAssetFailureMaximumBackoffMs = 30ull * 60'000;

struct MediaAssetFailure {
    MediaAssetKey key;
    u64 retry_at_ms;
    u32 failures;
    u32 reserved;
};

struct MediaAssetFailures {
    MediaAssetFailure entries[kMediaAssetFailureCapacity];
    u32 count;
    u32 recorded; // refusals recorded, repeats included
};

__declspec(noinline) inline MediaAssetFailure* media_asset_failure_find(const MediaAssetFailures* failures,
                                                                         const MediaAssetKey& key) noexcept {
    for (u32 i = 0; i < failures->count; ++i)
        if (media_asset_key_equal(failures->entries[i].key, key))
            return const_cast<MediaAssetFailure*>(&failures->entries[i]);
    return nullptr;
}

// Records a refusal of `key` at now_ms. True when the key had none recorded
// (the caller logs it); a full table replaces the entry due soonest.
__declspec(noinline) inline bool media_asset_failure_record(MediaAssetFailures* failures, const MediaAssetKey& key,
                                                            u64 now_ms) noexcept {
    ++failures->recorded;
    MediaAssetFailure* entry = media_asset_failure_find(failures, key);
    const bool first = !entry;
    if (!entry) {
        if (failures->count < kMediaAssetFailureCapacity) {
            entry = &failures->entries[failures->count++];
        } else {
            entry = &failures->entries[0];
            for (u32 i = 1; i < failures->count; ++i)
                if (failures->entries[i].retry_at_ms < entry->retry_at_ms)
                    entry = &failures->entries[i];
        }
        *entry = {key, 0, 0, 0};
    }
    const u32 shift = entry->failures < 6 ? entry->failures : 6;
    const u64 backoff = kMediaAssetFailureFirstBackoffMs << shift;
    entry->retry_at_ms = now_ms + (backoff < kMediaAssetFailureMaximumBackoffMs ? backoff : kMediaAssetFailureMaximumBackoffMs);
    ++entry->failures;
    return first;
}

// True while `key` waits out its backoff: no request may fetch or decode it.
inline bool media_asset_failure_blocked(const MediaAssetFailures& failures, const MediaAssetKey& key, u64 now_ms) noexcept {
    const MediaAssetFailure* entry = failures.count ? media_asset_failure_find(&failures, key) : nullptr;
    return entry && now_ms < entry->retry_at_ms;
}

// A later upload of `key` succeeded: its record goes.
inline void media_asset_failure_clear(MediaAssetFailures* failures, const MediaAssetKey& key) noexcept {
    MediaAssetFailure* entry = failures->count ? media_asset_failure_find(failures, key) : nullptr;
    if (entry)
        *entry = failures->entries[--failures->count];
}

} // namespace maicord
