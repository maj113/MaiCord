#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/media/asset_key.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// The selected server's banner above its channel list (the official client's
// server header banner): the list's top inset, the scroll-linked fade, the header
// glass while hovered, and an animated banner playing for 5 s when it comes
// into view (GIF autoplay) and while the strip below the header is hovered.

// Projects the banner onto the channel list scene (inset, header hover and
// revision); call before the scene updates. Rebuilds the list when the inset
// changed and the scene is live.
void application_guild_banner_project(u64 now_ms) noexcept;
// Pointer observer (never consumes): the header and the hover strip.
void application_guild_banner_pointer(UINT message, LPARAM lparam) noexcept;
// When the timed play ends (0 when nothing is pending).
u64 application_guild_banner_next_wake() noexcept;
// The pictures to keep resident while shown: the animation first while it
// plays, then the still it falls back to. url borrows state until the next call.
struct GuildBannerMedia {
    Utf8View url;
    MediaAssetKey key;
    bool play;
};
u32 application_guild_banner_media(GuildBannerMedia* output, u32 capacity) noexcept;
bool application_guild_banner_key_current(MediaAssetKey key) noexcept;
// A failed animated WebP switches to the CDN's GIF representation once.
void application_guild_banner_media_failed(MediaAssetKey key) noexcept;
// An animation that stopped or belongs to a server no longer shown: the pump
// releases its playback (frames and timeline). False when none is pending.
bool application_guild_banner_take_retired(MediaAssetKey* key) noexcept;
} // namespace maicord::app_detail
