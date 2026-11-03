#pragma once

#include "maicord/ui/chat/inbox_scene.hpp"
#include "maicord/ui/media/avatar_media.hpp"

namespace maicord {

// Mention card authors carry their decoded avatar hash, so authors absent from
// the user cache still resolve. Culled cards never admit media.
inline bool inbox_avatar_media_resolve(const UiInboxScene& scene, u32 slot, AvatarMedia* out) noexcept {
    UiRect rectangle{}, clip{};
    if (!out || !ui_inbox_scene_avatar(scene, slot, &rectangle, &clip))
        return false;
    const auto& row = scene.visible[slot];
    MediaAssetKey key;
    if (!row.author_id || !avatar_media_key(row.author_id, row.avatar_hash, &key))
        return false;
    *out = {key, row.avatar_hash, rectangle, clip};
    return true;
}

inline bool inbox_avatar_media_current(const UiInboxScene& scene, MediaAssetKey key) noexcept {
    return avatar_media_current(0, scene.count, key, [&](u32 slot, AvatarMedia* media) noexcept {
        return inbox_avatar_media_resolve(scene, slot, media);
    });
}

} // namespace maicord
