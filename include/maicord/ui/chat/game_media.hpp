#pragma once
#include "maicord/media/core/asset_key.hpp"
#include "maicord/protocol/guild/admin.hpp"

namespace maicord {

// A game's pictures as the official client asks for them: an asset hash from
// the CDN's app-icons path (PNG at the drawn size times the display scale,
// a proxy size), or an image URL, sized the same way when the CDN or the media
// proxy serves it (as is otherwise). Covers keep their aspect ratio.
inline constexpr u32 kGameImageUrlBytes = 384;
struct GameImageMedia {
    MediaAssetKey key; // profile_image identity of the exact URL
    u32 url_bytes;
    char url[kGameImageUrlBytes];
    Utf8View url_view() const noexcept { return {url, url_bytes}; }
};
bool game_icon_media(const GuildAdminGame& game, u32 size_dips, u32 dpi, GameImageMedia* output) noexcept;
bool game_cover_media(const GuildAdminGame& game, u32 size_dips, u32 dpi, GameImageMedia* output) noexcept;

struct GameStore;
// A game mention chip's icon: the known game's icon at the 32 px the
// official client asks for, whatever the display scale. False for a game not
// known or without an icon (the chip shows the placeholder glyph).
bool game_chip_icon_media(const GameStore* games, u64 id, GameImageMedia* output) noexcept;

} // namespace maicord
