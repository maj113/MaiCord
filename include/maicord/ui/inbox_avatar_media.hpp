#pragma once

#include "maicord/ui/inbox_scene.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {

struct InboxAvatarMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiWidgetReference owner;
    UiRect rectangle, clip;
};

// Mention card authors carry their decoded avatar hash, so authors absent from
// the user cache still resolve. Culled cards never admit media.
inline bool inbox_avatar_media_resolve(const UiInboxScene& scene, u32 slot, InboxAvatarMedia* out) noexcept {
    UiRect rectangle{}, clip{};
    if (!out || !ui_inbox_scene_avatar(scene, slot, &rectangle, &clip))
        return false;
    const auto& row = scene.visible[slot];
    const u32 variant = user_avatar_variant(row.avatar_hash);
    if (!row.author_id || !variant)
        return false;
    *out = {media_asset_user_avatar(row.author_id, variant, asset_hash_animated(row.avatar_hash)),
            row.avatar_hash, {&scene.widgets, scene.rows[slot]}, rectangle, clip};
    return true;
}

inline bool inbox_avatar_media_current(const UiInboxScene& scene, MediaAssetKey key) noexcept {
    for (u32 i = 0; i < scene.count && i < kUiInboxVisibleRows; ++i) {
        InboxAvatarMedia media{};
        if (inbox_avatar_media_resolve(scene, i, &media) && media_asset_key_equal(media.key, key))
            return true;
    }
    return false;
}

} // namespace maicord
