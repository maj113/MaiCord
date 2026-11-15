#pragma once
#include "maicord/media/core/asset_key.hpp"
#include "maicord/ui/core/geometry.hpp"
namespace maicord {
struct UiLayoutNode;

// Resolved member row media, keyed by user, with keys and geometry relative to the
// row's top-left corner so a row that only moved (scrolling) is not resolved again.
// The plain (shared) avatar takes the avatar slot when the member has no per-guild
// avatar. A change in any store revision the resolution reads, the measure cache
// generation or the guild clears the cache; hover, offline state and row width
// revalidate a single row.
inline constexpr u32 kMemberMediaLayerCount = 7;
inline constexpr u32 kMemberMediaCacheCapacity = 128;
inline constexpr u32 kMemberMediaRowMemory = 64;
struct MemberMediaCacheEntry {
    // The probe reads the key, width and state together: they share the first cache line.
    u64 user_id;
    float width;
    u8 state;
    u8 layer_mask;
    u8 circular_mask;
    bool plain_avatar;
    bool badge;
    bool badge_leading, badge_accent;
    u8 badge_bytes;
    char badge_text[32]; // A copy: a replaced store can repeat the revisions in the stamp.
    MediaAssetKey keys[kMemberMediaLayerCount];
    UiRect rectangles[kMemberMediaLayerCount];
    UiRect badge_rectangle;
};
struct MemberMediaCache {
    u64 stamp;
    u32 count;
    u32 resolves; // Rows resolved into the cache.
    u32 hits;     // Row lookups served by it.
    // Per visible row (by index): the lookup key, node and entry slot, so all
    // layers of every row share one residency check while nothing the lookup
    // reads has moved (the media scans visit each row twice per frame).
    u32 row_slots[kMemberMediaRowMemory]; // ~0u: the row has no media.
    u64 row_keys[kMemberMediaRowMemory];
    const UiLayoutNode* row_nodes[kMemberMediaRowMemory];
    MemberMediaCacheEntry entries[kMemberMediaCacheCapacity];
};
void member_media_cache_reset(MemberMediaCache* cache) noexcept;
} // namespace maicord
