#pragma once

#include "maicord/ui/chat/polls/controls.hpp"
#include "maicord/ui/media/avatar_media.hpp"

namespace maicord {

// Voter rows carry the avatar hash decoded from the voters page.
inline bool poll_voters_avatar_media_resolve(const UiPollVotersScene& scene, u32 index,
                                             AvatarMedia* out) noexcept {
    UiRect rectangle{}, clip{};
    if (!out || !ui_poll_voters_avatar(scene, index, &rectangle, &clip))
        return false;
    const auto& voter = scene.state.voters[index];
    MediaAssetKey key;
    if (!voter.id || !avatar_media_key(voter.id, voter.avatar_hash, &key))
        return false;
    *out = {key, voter.avatar_hash, rectangle, clip};
    return true;
}

inline bool poll_voters_avatar_media_current(const UiPollVotersScene& scene, MediaAssetKey key) noexcept {
    const u32 end = minimum<u32>(scene.first + scene.visible, scene.state.voters.count);
    return avatar_media_current(scene.first, end, key, [&](u32 index, AvatarMedia* media) noexcept {
        return poll_voters_avatar_media_resolve(scene, index, media);
    });
}

} // namespace maicord
