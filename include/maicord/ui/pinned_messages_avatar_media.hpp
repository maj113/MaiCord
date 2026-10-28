#pragma once

#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {

struct PinnedMessagesAvatarMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiRect rectangle, clip;
};
static_assert(sizeof(PinnedMessagesAvatarMedia) <= 80);

// Borrow only the exact accepted page's visible author identity. The scene
// validates its revision/authority and culls the avatar before admitting media.
inline bool pinned_messages_avatar_media_resolve(const UiPinnedMessagesScene& scene, u32 slot,
                                                 PinnedMessagesAvatarMedia* out) noexcept {
    UiRect rectangle{}, clip{};
    if (!out || !ui_pinned_messages_scene_avatar(scene, slot, &rectangle, &clip))
        return false;
    const auto& row = scene.visible[slot];
    const u32 variant = user_avatar_variant(row.avatar_hash);
    if (!row.author_id || !variant)
        return false;
    *out = {media_asset_user_avatar(row.author_id, variant,
                                   asset_hash_animated(row.avatar_hash)),
            row.avatar_hash, rectangle, clip};
    return true;
}

inline bool pinned_messages_avatar_media_current(const UiPinnedMessagesScene& scene,
                                                 MediaAssetKey key) noexcept {
    for (u32 i = 0; i < scene.count && i < kUiPinnedMessagesVisibleRows; ++i) {
        PinnedMessagesAvatarMedia media{};
        if (pinned_messages_avatar_media_resolve(scene, i, &media) &&
            media_asset_key_equal(media.key, key))
            return true;
    }
    return false;
}

} // namespace maicord
