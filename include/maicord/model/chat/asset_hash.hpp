#pragma once

#include "maicord/core/ascii.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {

__declspec(noinline) constexpr bool asset_hash_animated(const Utf8View& hash) noexcept {
    return hash.bytes == 34 && hash.data && hash.data[0] == 'a' && hash.data[1] == '_';
}

// 32 hex digits, after an a_ prefix when animated.
inline bool asset_hash_hex_valid(const Utf8View& hash) noexcept {
    const u32 begin = asset_hash_animated(hash) ? 2u : 0u;
    if (!hash.data || hash.bytes - begin != 32)
        return false;
    for (u32 index = begin; index < hash.bytes; ++index)
        if (!ascii_is_hex_digit(hash.data[index]))
            return false;
    return true;
}

// The nonzero 32-bit FNV-1a of an asset hash: the key that tells cached images of one asset's revisions apart.
inline u32 asset_hash_variant(const Utf8View& hash) noexcept {
    u32 value = 2'166'136'261u;
    #pragma clang loop unroll(disable) // a dependent multiply chain: unrolling only adds code
    for (u32 index = 0; index < hash.bytes; ++index) {
        value ^= static_cast<u8>(hash.data[index]);
        value *= 16'777'619u;
    }
    return value == 0 ? 1u : value;
}

} // namespace maicord
