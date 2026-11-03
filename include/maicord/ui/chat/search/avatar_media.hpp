#pragma once
#include "maicord/ui/chat/search/scene.hpp"
#include "maicord/ui/media/avatar_media.hpp"

namespace maicord {
// Result rows take the first slots; the from:/mentions: people suggestions of
// the open chooser follow them.
inline constexpr u32 kSearchAvatarMediaSlots = kUiSearchVisibleRows + kUiSearchFilterChoiceMaximum;
inline bool search_choice_avatar_media_resolve(const UiSearchScene& scene, u32 index,
                                               AvatarMedia* out) noexcept {
    const auto* source = scene.choice_source;
    if (!out || !source || index >= source->rows.count)
        return false;
    const auto& row = source->rows[index];
    MediaAssetKey key;
    UiRect rectangle{}, clip{};
    if (!row.id || !avatar_media_key(row.id, row.avatar_hash, &key) ||
        !ui_search_scene_filter_choice_avatar(scene, index, &rectangle, &clip))
        return false;
    *out = {key, row.avatar_hash, rectangle, clip};
    return true;
}
inline bool search_avatar_media_resolve(const UiSearchScene& scene, u32 slot,
                                        AvatarMedia* out) noexcept {
    if (slot >= kUiSearchVisibleRows)
        return search_choice_avatar_media_resolve(scene, slot - kUiSearchVisibleRows, out);
    UiRect rectangle{}, clip{};
    if (!out || !ui_search_scene_avatar(scene, slot, &rectangle, &clip))
        return false;
    const auto& row = scene.visible[slot];
    MediaAssetKey key;
    if (!row.author_id || !avatar_media_key(row.author_id, row.avatar_hash, &key))
        return false;
    if (const auto* native = ui_search_scene_message_scene(scene);
        native && !message_scene_find_row(*native, scene.first + slot))
        return false;
    *out = {key, row.avatar_hash, rectangle, clip};
    return true;
}
inline bool search_avatar_media_current(const UiSearchScene& scene, MediaAssetKey key) noexcept {
    return avatar_media_current(0, kSearchAvatarMediaSlots, key, [&](u32 slot, AvatarMedia* media) noexcept {
        return search_avatar_media_resolve(scene, slot, media);
    });
}
} // namespace maicord
