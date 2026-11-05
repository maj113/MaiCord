#pragma once

#include "maicord/media/core/asset_key.hpp"
#include "maicord/render/media/atlas.hpp"

namespace maicord {

// An upload the atlas refused for want of room (the visible set fills it). The
// decoded frame is gone once the upload is refused, so asking the account for
// the same key again decodes it again: without this memory every pump pass
// would decode, fail to upload and stop, and nothing behind it in the scan
// would load. The key waits until the atlas could take its size (something
// scrolled away, a shelf was packed or grown), or for a safety interval in case
// the prediction is wrong.
struct MediaCapacityDeferral {
    MediaAssetKey key;
    u64 retry_at_ms;  // Safety: the key may ask again from here whatever the atlas looks like.
    u64 earliest_ms;  // Never earlier: a prediction that the atlas fits must not become a decode per pass.
    u16 width;
    u16 height;
};

inline constexpr u32 kMediaCapacityDeferralCapacity = 64;
inline constexpr u32 kMediaCapacityDeferralSafetyMs = 10'000;
inline constexpr u32 kMediaCapacityDeferralMinimumMs = 1'000;

struct MediaCapacityDeferrals {
    MediaCapacityDeferral entries[kMediaCapacityDeferralCapacity];
    u32 cursor;
    // Counters for the capacity diagnostics.
    u32 refused;  // uploads the atlas refused
    u32 waited;   // requests held back while a key waited
    u32 released; // keys allowed to ask again
};

void media_capacity_defer(MediaCapacityDeferrals* deferrals, MediaAssetKey key, u16 width, u16 height,
                          u64 now_ms) noexcept;
// True while `key` waits. `wake_ms` (optional) receives when the wait ends at the latest.
bool media_capacity_waiting(MediaCapacityDeferrals* deferrals, const MediaAtlas& atlas, MediaAssetKey key,
                            u64 now_ms, u64* wake_ms = nullptr) noexcept;
u32 media_capacity_waiting_count(const MediaCapacityDeferrals& deferrals) noexcept;

} // namespace maicord
