#pragma once
#include "maicord/ui/friends_scene.hpp"
#include "maicord/media/asset_key.hpp"

namespace maicord {
struct UserStore;
// One borrowed viewport candidate; no name, image or hash bank is retained.
struct FriendsAvatarMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiRect rectangle, clip;
};
static_assert(sizeof(FriendsAvatarMedia) <= 80);
// Slots cover the visible rows, then the Active Now column's avatars.
u32 friends_avatar_media_count(const UiFriendsScene&) noexcept;
bool friends_avatar_media_resolve(const UiFriendsScene&, const UserStore&,
    u32 visible_slot, FriendsAvatarMedia*) noexcept;
bool friends_avatar_media_current(const UiFriendsScene&, const UserStore&,
    MediaAssetKey) noexcept;
struct FriendsAvatarMediaCallbacks {
    void* context;
    bool (*resident)(void*, MediaAssetKey) noexcept;
    bool (*observe_animation)(void*, MediaAssetKey) noexcept;
    bool (*request)(void*, const FriendsAvatarMedia&, bool force, bool* stop) noexcept;
};
struct FriendsAvatarMediaVisitResult {
    u32 visited, candidates, requests;
    bool ok, stopped;
};
// Only retained rows intersecting the current viewport can reach a request.
FriendsAvatarMediaVisitResult friends_avatar_media_visit(const UiFriendsScene&,
    const UserStore&, const FriendsAvatarMediaCallbacks&) noexcept;
} // namespace maicord
