#pragma once

#include "maicord/ui/poll_controls.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {

struct PollVotersAvatarMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiRect rectangle, clip;
};

// Voter rows carry the avatar hash decoded from the voters page.
inline bool poll_voters_avatar_media_resolve(const UiPollVotersScene& scene, u32 index,
                                             PollVotersAvatarMedia* out) noexcept {
    UiRect rectangle{}, clip{};
    if (!out || !ui_poll_voters_avatar(scene, index, &rectangle, &clip))
        return false;
    const auto& voter = scene.state.voters[index];
    const u32 variant = user_avatar_variant(voter.avatar_hash);
    if (!voter.id || !variant)
        return false;
    *out = {media_asset_user_avatar(voter.id, variant, asset_hash_animated(voter.avatar_hash)), voter.avatar_hash,
            rectangle, clip};
    return true;
}

inline bool poll_voters_avatar_media_current(const UiPollVotersScene& scene, MediaAssetKey key) noexcept {
    for (u32 i = scene.first; i < scene.first + scene.visible && i < scene.state.voters.count; ++i) {
        PollVotersAvatarMedia media{};
        if (poll_voters_avatar_media_resolve(scene, i, &media) && media_asset_key_equal(media.key, key))
            return true;
    }
    return false;
}

} // namespace maicord
