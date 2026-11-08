#include "maicord/model/chat/asset_hash.hpp"

// Separate object: the client needs this check for every Gateway user record,
// while the rest of cdn_url.cpp lives only in the account module.
namespace maicord {

bool guild_icon_hash_valid(Utf8View icon_hash) noexcept {
    return asset_hash_hex_valid(icon_hash);
}

} // namespace maicord
