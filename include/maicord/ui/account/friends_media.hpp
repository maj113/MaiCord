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
// Active Now's activity images (art and its corner badge) over every shown
// card in order, placed on the card and clipped to it; false past the last.
bool friends_activity_media_resolve(const UiFriendsScene&, u32 index, ProfileMedia*) noexcept;
bool friends_activity_media_current(const UiFriendsScene&, MediaAssetKey) noexcept;
} // namespace maicord
