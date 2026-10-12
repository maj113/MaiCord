#pragma once

#include "maicord/model/chat_state.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {

// A server's identity image: the still media key and CDN hash the server
// rail uses at rest, so every surface that shows a server tile shares one
// resident icon and one request. The official client never animates these
// tiles, so an a_ hash also names the CDN's still (.webp without
// animated=true). False when the server has no icon; the surface keeps its
// placeholder (initials or glyph). hash borrows the state store.
inline bool guild_icon_media_key(const ChatStateStore& state, u64 guild_id, MediaAssetKey* key,
                                 Utf8View* hash) noexcept {
    const auto* guild = guild_id ? chat_state_store_find_guild(state, guild_id) : nullptr;
    if (!guild || !guild->icon_variant || !guild->icon_hash_bytes)
        return false;
    const Utf8View icon = chat_state_store_guild_icon_hash(*guild);
    if (!icon.bytes)
        return false;
    if (key)
        *key = media_asset_guild_icon(guild->id, guild->icon_variant, false);
    if (hash)
        *hash = icon;
    return true;
}

} // namespace maicord
