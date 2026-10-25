#include "maicord/core/ascii.hpp"
#include "maicord/model/asset_hash.hpp"

// Separate object: the client needs this check for every Gateway user record,
// while the rest of cdn_url.cpp lives only in the account module.
namespace maicord {

bool guild_icon_hash_valid(Utf8View icon_hash) noexcept {
    const u32 begin = asset_hash_animated(icon_hash) ? 2u : 0u;
    if (!icon_hash.data || icon_hash.bytes - begin != 32) {
        return false;
    }
    for (u32 index = begin; index < icon_hash.bytes; ++index) {
        if (!ascii_is_hex_digit(icon_hash.data[index])) {
            return false;
        }
    }
    return true;
}

} // namespace maicord
