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

// The user-avatar key for a custom avatar hash; false for a default or
// malformed hash, which keeps the native placeholder. user_id must be non-zero.
__forceinline bool avatar_media_key(u64 user_id, const Utf8View& hash, MediaAssetKey* key) noexcept {
    const u32 variant = user_avatar_variant(hash);
    if (!variant)
        return false;
    *key = media_asset_user_avatar(user_id, variant, asset_hash_animated(hash));
    return true;
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
