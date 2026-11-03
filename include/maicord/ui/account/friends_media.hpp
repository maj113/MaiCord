#pragma once
#include "maicord/ui/account/friends_scene.hpp"
#include "maicord/ui/media/avatar_media.hpp"

namespace maicord {
// Slots cover the visible rows, then the Active Now column's avatars.
u32 friends_avatar_media_count(const UiFriendsScene&) noexcept;
bool friends_avatar_media_resolve(const UiFriendsScene&, const UserStore&,
    u32 visible_slot, AvatarMedia*) noexcept;
bool friends_avatar_media_current(const UiFriendsScene&, const UserStore&,
    MediaAssetKey) noexcept;
struct FriendsAvatarMediaCallbacks {
    void* context;
    bool (*resident)(void*, MediaAssetKey) noexcept;
    bool (*observe_animation)(void*, MediaAssetKey) noexcept;
    bool (*request)(void*, const AvatarMedia&, bool force, bool* stop) noexcept;
};
struct FriendsAvatarMediaVisitResult {
    u32 visited, candidates, requests;
    bool ok, stopped;
};
// Only retained rows intersecting the current viewport can reach a request.
FriendsAvatarMediaVisitResult friends_avatar_media_visit(const UiFriendsScene&,
    const UserStore&, const FriendsAvatarMediaCallbacks&) noexcept;
} // namespace maicord
