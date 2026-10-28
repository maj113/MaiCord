#pragma once
#include "maicord/ui/search_scene.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
struct SearchAvatarMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiRect rectangle, clip;
};
static_assert(sizeof(SearchAvatarMedia) <= 80);
// Result rows take the first slots; the from:/mentions: people suggestions of
// the open chooser follow them.
inline constexpr u32 kSearchAvatarMediaSlots = kUiSearchVisibleRows + kUiSearchFilterChoiceMaximum;
inline bool search_choice_avatar_media_resolve(const UiSearchScene& scene, u32 index,
                                               SearchAvatarMedia* out) noexcept {
    const auto* source = scene.choice_source;
    if (!out || !source || index >= source->rows.count)
        return false;
    const auto& row = source->rows[index];
    const u32 variant = user_avatar_variant(row.avatar_hash);
    UiRect rectangle{}, clip{};
    if (!row.id || !variant || !ui_search_scene_filter_choice_avatar(scene, index, &rectangle, &clip))
        return false;
    *out = {media_asset_user_avatar(row.id, variant, asset_hash_animated(row.avatar_hash)), row.avatar_hash,
            rectangle, clip};
    return true;
}
inline bool search_avatar_media_resolve(const UiSearchScene& scene, u32 slot,
                                        SearchAvatarMedia* out) noexcept {
    if (slot >= kUiSearchVisibleRows)
        return search_choice_avatar_media_resolve(scene, slot - kUiSearchVisibleRows, out);
    UiRect rectangle{}, clip{};
    if (!out || !ui_search_scene_avatar(scene, slot, &rectangle, &clip))
        return false;
    const auto& row = scene.visible[slot];
    const u32 variant = user_avatar_variant(row.avatar_hash);
    if (!row.author_id || !variant)
        return false;
    if (const auto* native = ui_search_scene_message_scene(scene);
        native && !message_scene_find_row(*native, scene.first + slot))
        return false;
    *out = {media_asset_user_avatar(row.author_id, variant, asset_hash_animated(row.avatar_hash)),
            row.avatar_hash,
            rectangle,
            clip};
    return true;
}
inline bool search_avatar_media_current(const UiSearchScene& scene, MediaAssetKey key) noexcept {
    for (u32 i = 0; i < kSearchAvatarMediaSlots; ++i) {
        SearchAvatarMedia media{};
        if (search_avatar_media_resolve(scene, i, &media) && media_asset_key_equal(media.key, key))
            return true;
    }
    return false;
}
} // namespace maicord
