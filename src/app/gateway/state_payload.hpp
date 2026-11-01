#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/core/ascii.hpp"

namespace maicord {

inline bool gateway_state_emoji_name_valid(Utf8View name) noexcept {
    if (!name.data || name.bytes == 0 || name.bytes > 32 || !utf8_validate(name).valid) {
        return false;
    }
    for (u32 index = 0; index < name.bytes; ++index) {
        const u8 value = static_cast<u8>(name.data[index]);
        if (!(ascii_is_alnum(value) || value == '_')) {
            return false;
        }
    }
    return true;
}

} // namespace maicord
