#include "maicord/ui/friends_media.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {
u32 friends_avatar_media_count(const UiFriendsScene& scene) noexcept {
    return ui_friends_scene_visible_count(scene) + ui_friends_scene_active_avatar_count(scene);
}
bool friends_avatar_media_resolve(const UiFriendsScene& scene, const UserStore& users,
    u32 slot, FriendsAvatarMedia* output) noexcept {
    if (!output) return false;
    u64 user_id = 0;
    UiWidgetReference owner{};
    UiRect avatar{}, clip{};
    const u32 rows = ui_friends_scene_visible_count(scene);
    if (slot < rows) {
        UiFriendsVisibleRow visible{};
        if (!ui_friends_scene_visible_row(scene, slot, &visible)) return false;
        user_id = visible.row.user_id;owner = visible.owner;avatar = visible.avatar;clip = visible.clip;
    } else {
        UiFriendsActiveAvatar active{};
        if (!ui_friends_scene_active_avatar(scene, slot - rows, &active)) return false;
        user_id = active.user_id;owner = {&scene.widgets, scene.heading};avatar = active.rectangle;clip = active.clip;
    }
    if (!ui_rect_has_area(ui_rect_intersection(avatar, clip))) return false;
    const auto* user = user_store_find(users, user_id);
    if (!user) return false;
    const auto hash = user_store_avatar_hash(*user);
    const u32 variant = user_avatar_variant(hash);
    if (!variant) return false; // Defaults and malformed hashes keep the native placeholder.
    *output = {media_asset_user_avatar(user->id, variant, asset_hash_animated(hash)), hash, owner, avatar, clip};
    return true;
}
bool friends_avatar_media_current(const UiFriendsScene& scene, const UserStore& users,
    MediaAssetKey key) noexcept {
    if (key.kind != MediaAssetKind::user_avatar || !media_asset_key_valid(key)) return false;
    const u32 count = friends_avatar_media_count(scene);
    if (count > kUiFriendsMaximumVisibleRows + kUiFriendsActiveNowCards * kUiFriendsActiveNowAvatars) return false;
    for (u32 i = 0; i < count; ++i) {
        FriendsAvatarMedia media{};
        if (friends_avatar_media_resolve(scene, users, i, &media) && media_asset_key_equal(media.key, key)) return true;
    }
    return false;
}
FriendsAvatarMediaVisitResult friends_avatar_media_visit(const UiFriendsScene& scene,
    const UserStore& users, const FriendsAvatarMediaCallbacks& callbacks) noexcept {
    FriendsAvatarMediaVisitResult result{0, 0, 0, true, false};
    if (!callbacks.resident || !callbacks.observe_animation || !callbacks.request) { result.ok = false;return result; }
    const u32 count = friends_avatar_media_count(scene);
    if (count > kUiFriendsMaximumVisibleRows + kUiFriendsActiveNowCards * kUiFriendsActiveNowAvatars) { result.ok = false;return result; }
    for (u32 i = 0; i < count; ++i) {
        ++result.visited;
        FriendsAvatarMedia media{};
        if (!friends_avatar_media_resolve(scene, users, i, &media)) continue;
        ++result.candidates;
        const bool resident = callbacks.resident(callbacks.context, media.key);
        const bool animated = (media.key.flags & media_asset_flag_animated) != 0;
        const bool observed = animated && callbacks.observe_animation(callbacks.context, media.key);
        if (resident && (!animated || observed)) continue;
        bool stop = false;
        ++result.requests;
        if (!callbacks.request(callbacks.context, media, animated && !observed, &stop)) { result.ok = false;return result; }
        if (stop) { result.stopped = true;return result; }
    }
    return result;
}
} // namespace maicord
