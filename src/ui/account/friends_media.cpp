#include "maicord/ui/account/friends_media.hpp"
#include "maicord/ui/account/profile_media.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
u32 friends_avatar_media_count(const UiFriendsScene& scene) noexcept {
    return ui_friends_scene_visible_count(scene) + ui_friends_scene_active_avatar_count(scene);
}
bool friends_avatar_media_resolve(const UiFriendsScene& scene, const UserStore& users,
    u32 slot, AvatarMedia* output) noexcept {
    if (!output) return false;
    u64 user_id = 0;
    UiRect avatar{}, clip{};
    const u32 rows = ui_friends_scene_visible_count(scene);
    if (slot < rows) {
        UiFriendsVisibleRow visible{};
        if (!ui_friends_scene_visible_row(scene, slot, &visible)) return false;
        user_id = visible.row.user_id;avatar = visible.avatar;clip = visible.clip;
    } else {
        UiFriendsActiveAvatar active{};
        if (!ui_friends_scene_active_avatar(scene, slot - rows, &active)) return false;
        user_id = active.user_id;avatar = active.rectangle;clip = active.clip;
    }
    if (!ui_rect_intersects(avatar, clip)) return false;
    const auto* user = user_store_find(users, user_id);
    if (!user) return false;
    const auto hash = user_store_avatar_hash(*user);
    MediaAssetKey key;
    // A malformed hash keeps the native placeholder; none shows the default avatar.
    if (!avatar_media_key(user->id, hash, &key)) return false;
    *output = {key, hash, avatar, clip};
    return true;
}
bool friends_avatar_media_current(const UiFriendsScene& scene, const UserStore& users,
    MediaAssetKey key) noexcept {
    if ((key.kind != MediaAssetKind::user_avatar && !is_default_avatar_key(key)) || !media_asset_key_valid(key)) return false;
    const u32 count = friends_avatar_media_count(scene);
    if (count > kUiFriendsMaximumVisibleRows + kUiFriendsActiveNowCards * kUiFriendsActiveNowAvatars) return false;
    return avatar_media_current(0, count, key, [&](u32 slot, AvatarMedia* media) noexcept {
        return friends_avatar_media_resolve(scene, users, slot, media);
    });
}
FriendsAvatarMediaVisitResult friends_avatar_media_visit(const UiFriendsScene& scene,
    const UserStore& users, const FriendsAvatarMediaCallbacks& callbacks) noexcept {
    FriendsAvatarMediaVisitResult result{0, 0, 0, true, false};
    if (!callbacks.resident || !callbacks.observe_animation || !callbacks.request) { result.ok = false;return result; }
    const u32 count = friends_avatar_media_count(scene);
    if (count > kUiFriendsMaximumVisibleRows + kUiFriendsActiveNowCards * kUiFriendsActiveNowAvatars) { result.ok = false;return result; }
    for (u32 i = 0; i < count; ++i) {
        ++result.visited;
        AvatarMedia media{};
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
bool friends_activity_media_resolve(const UiFriendsScene& scene, u32 index, ProfileMedia* output) noexcept {
    if (!output || !scene.active_now) return false;
    UiRect card{};
    UiPoint origin{};
    for (u32 i = 0; ui_friends_scene_active_card(scene, i, &card, &origin); ++i) {
        const auto& data = scene.active_now->cards[i];
        if (!data.activity || !data.images) continue;
        if (index >= data.image_count) {
            index -= data.image_count;
            continue;
        }
        copy_value(output, data.images[index]);
        const auto place = [&](UiRect& r) noexcept {
            r = {r.left + origin.x, r.top + origin.y, r.right + origin.x, r.bottom + origin.y};
        };
        place(output->rectangle);
        if (ui_rect_has_area(output->mask)) place(output->mask);
        output->clip = ui_rect_intersection(card, scene.active_rect);
        return true;
    }
    return false;
}
bool friends_activity_media_current(const UiFriendsScene& scene, MediaAssetKey key) noexcept {
    ProfileMedia media;
    for (u32 i = 0; friends_activity_media_resolve(scene, i, &media); ++i)
        if (media_asset_key_equal(key, media.key) && ui_rect_intersects(media.rectangle, media.clip)) return true;
    return false;
}
} // namespace maicord
