#pragma once
#include "maicord/ui/profile_scene.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/ui/member_list_scene.hpp"
namespace maicord {
struct ProfileMedia {
    u64 playback_epoch;
    MediaAssetKey key;
    ProfileString<384> url;
    UiRect rectangle, clip;
    bool circular;
    // Local images (a picked avatar or banner before upload) live in the
    // preview atlas (domain 1) and are never fetched by URL. A mask with area
    // rounds the drawn image to that rectangle (mask_radius; half its width
    // makes a circle) instead of the quad itself: a cropped image shows only
    // its visible part. Zero opacity draws fully opaque.
    u8 domain;
    float opacity;
    UiRect mask;
    float mask_radius;
    // Clockwise turn of the image inside its rectangle (0, 90, 180, 270).
    u16 rotation;
};
bool profile_media_resolve(const ProfileScene& scene, u32 index, ProfileMedia* output) noexcept;
bool user_guild_badge_media(const UserAppearance&, UiRect rectangle, UiRect clip, ProfileMedia*) noexcept;
bool user_guild_badge_media(const UserAppearanceRecord&, UiRect rectangle, UiRect clip,
                            ProfileMedia*) noexcept;
// Media representation changes do not invalidate retained text/widget handles.
bool profile_media_use_compatibility_banner(ProfileScene* scene) noexcept;
enum class MemberMedia : u8 {
    nameplate,
    avatar,
    decoration,
    guild_badge,
    status_emoji
};
// MemberMedia::avatar resolves only a per-guild avatar (its own URL identity).
// visible_only false also resolves rows outside the viewport.
bool member_media_resolve(const MemberListScene& scene, u32 index, MemberMedia layer,
                          ProfileMedia* output, bool visible_only = true) noexcept;
// A visible member row without a guild avatar draws the shared user-avatar
// identity, requested and cached like message authors.
struct MemberAvatarMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiRect rectangle, clip;
};
bool member_plain_avatar_resolve(const MemberListScene& scene, u32 index, MemberAvatarMedia* output,
                                 bool visible_only = true) noexcept;
bool member_plain_avatar_current(const MemberListScene& scene, MediaAssetKey key) noexcept;

// Resolved member row media, keyed by user: keys and geometry relative to the
// row's top-left corner, so a row that only moved (scrolling) is not resolved
// again. The plain (shared) avatar takes the avatar slot when the member has
// no per-guild avatar. Every store revision the resolution reads, the measure
// cache generation and the guild clear the cache; hover and offline state and
// the row width revalidate a single row.
inline constexpr u32 kMemberMediaLayerCount = 5;
inline constexpr u32 kMemberMediaCacheCapacity = 128;
inline constexpr u32 kMemberMediaRowMemory = 64;
struct MemberMediaCacheEntry {
    u64 user_id;
    float width;
    u8 state;
    u8 layer_mask;
    u8 circular_mask;
    bool plain_avatar;
    bool badge;
    bool badge_leading, badge_accent;
    MediaAssetKey keys[kMemberMediaLayerCount];
    UiRect rectangles[kMemberMediaLayerCount];
    UiRect badge_rectangle;
    u8 badge_bytes;
    char badge_text[32]; // A copy: a replaced store can repeat the revisions in the stamp.
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
struct MemberMediaPlacement {
    MediaAssetKey key;
    UiRect rectangle, clip;
    bool circular;
};
// The cached row (null for group, unresident or empty rows); *origin is the
// row's top-left corner. Without a scene cache it is always null.
const MemberMediaCacheEntry* member_media_cache_row(const MemberListScene& scene, u32 index,
                                                    UiPoint* origin) noexcept;
// member_media_resolve without the URL, through the scene cache when it has one.
bool member_media_place(const MemberListScene& scene, u32 index, MemberMedia layer,
                        MemberMediaPlacement* output) noexcept;
// member_plain_avatar_resolve without the hash, through the scene cache.
bool member_plain_avatar_place(const MemberListScene& scene, u32 index, MemberMediaPlacement* output) noexcept;
bool profile_effect_media_resolve(const ProfileScene& scene, u32 index, u64 now_ms,
                                  ProfileMedia* output) noexcept;
} // namespace maicord
