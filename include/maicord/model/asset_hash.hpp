#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

constexpr bool asset_hash_animated(Utf8View hash) noexcept {
    return hash.bytes == 34 && hash.data && hash.data[0] == 'a' && hash.data[1] == '_';
}

} // namespace maicord
