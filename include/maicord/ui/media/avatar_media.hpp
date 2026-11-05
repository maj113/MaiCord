#pragma once

#include "maicord/media/core/asset.hpp"
#include "maicord/model/chat/asset_hash.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/ui/core/geometry.hpp"

namespace maicord {

// One borrowed visible avatar candidate: the shared user-avatar identity, the
// hash it came from (still owned by the scene or user store) and its placement.
// No name, image or hash bank is retained.
struct AvatarMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiRect rectangle, clip;
};
static_assert(sizeof(AvatarMedia) <= 80);

// The CDN's default avatars: six pictures, picked by (user id >> 22) % 6 for every account on the username
// system. Each is fetched by URL from the CDN (nothing is bundled) under one shared key, so every surface that
// shows a user without an avatar, the profile card and Settings included, draws the same atlas entry.
inline constexpr u32 kDefaultAvatarCount = 6;
inline constexpr u32 kDefaultAvatarPathBytes = 19;
inline constexpr u64 kDefaultAvatarKeyBase = 0xD3FA17A800000000ull;

constexpr u32 default_avatar_index(u64 user_id) noexcept {
    return static_cast<u32>((user_id >> 22) % kDefaultAvatarCount);
}

constexpr MediaAssetKey default_avatar_key(u32 index) noexcept {
    MediaAssetKey key = media_asset_embed(kDefaultAvatarKeyBase + index, 1, 0, MediaEmbedResource::image,
                                          MediaAttachmentFormat::png, false, MediaResolutionTier::px512);
    key.kind = MediaAssetKind::profile_image;
    return key;
}

constexpr bool is_default_avatar_key(const MediaAssetKey& key) noexcept {
    return key.kind == MediaAssetKind::profile_image && key.id - kDefaultAvatarKeyBase < kDefaultAvatarCount;
}

// "embed/avatars/N.png": the path of a default avatar under https://cdn.discordapp.com/.
inline void default_avatar_path(u32 index, char (&path)[kDefaultAvatarPathBytes]) noexcept {
    static constexpr char head[] = "embed/avatars/";
    static constexpr char tail[] = ".png";
    u32 at = 0;
    for (char byte : head)
        if (byte)
            path[at++] = byte;
    path[at++] = static_cast<char>('0' + index);
    for (char byte : tail)
        if (byte)
            path[at++] = byte;
}

// The key a user's avatar draws under: a valid hash's own, the default avatar's when there is none, and an
// empty key (the native placeholder) for a malformed hash. user_id must be non-zero.
__forceinline MediaAssetKey user_avatar_key(u64 user_id, const Utf8View& hash) noexcept {
    if (!hash.bytes)
        return default_avatar_key(default_avatar_index(user_id));
    const u32 variant = user_avatar_variant(hash);
    return variant ? media_asset_user_avatar(user_id, variant, asset_hash_animated(hash)) : MediaAssetKey{};
}

// The key for an avatar hash (empty: the default avatar); false for a malformed hash, which keeps the native
// placeholder. user_id must be non-zero.
__forceinline bool avatar_media_key(u64 user_id, const Utf8View& hash, MediaAssetKey* key) noexcept {
    *key = user_avatar_key(user_id, hash);
    return key->kind != MediaAssetKind::none;
}

// Whether resolve(slot, &media) admits `key` for a slot in [first, end).
template <class Resolve>
inline bool avatar_media_current(u32 first, u32 end, MediaAssetKey key, Resolve resolve) noexcept {
    for (u32 slot = first; slot < end; ++slot) {
        AvatarMedia media;
        if (resolve(slot, &media) && media_asset_key_equal(media.key, key))
            return true;
    }
    return false;
}

} // namespace maicord
