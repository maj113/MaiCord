#pragma once

#include "maicord/ui/chat/pinned_messages_scene.hpp"
#include "maicord/ui/media/avatar_media.hpp"

namespace maicord {

// Borrow only the exact accepted page's visible author identity. The scene
// validates its revision/authority and culls the avatar before admitting media.
inline bool pinned_messages_avatar_media_resolve(const UiPinnedMessagesScene& scene, u32 slot,
                                                 AvatarMedia* out) noexcept {
    UiRect rectangle{}, clip{};
    if (!out || !ui_pinned_messages_scene_avatar(scene, slot, &rectangle, &clip))
        return false;
    const auto& row = scene.visible[slot];
    MediaAssetKey key;
    if (!row.author_id || !avatar_media_key(row.author_id, row.avatar_hash, &key))
        return false;
    *out = {key, row.avatar_hash, rectangle, clip};
    return true;
}

inline bool pinned_messages_avatar_media_current(const UiPinnedMessagesScene& scene,
                                                 MediaAssetKey key) noexcept {
    const u32 end = minimum<u32>(scene.count, kUiPinnedMessagesVisibleRows);
    return avatar_media_current(0, end, key, [&](u32 slot, AvatarMedia* media) noexcept {
        return pinned_messages_avatar_media_resolve(scene, slot, media);
    });
}

} // namespace maicord
