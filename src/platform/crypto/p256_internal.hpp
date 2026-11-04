#pragma once

#include "maicord/platform/crypto/p256.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace p256_internal {
namespace {

// The argument checks of the P-256 calls, shared by the Windows and Linux backends: they run before any key
// object is created or used.
template <typename T> ConstSlice<u8> bytes(const T& value) noexcept {
    return {reinterpret_cast<const u8*>(&value), sizeof(value)};
}
bool scalar_valid(ConstSlice<u8> scalar) noexcept {
    static constexpr u8 order[]{0xff, 0xff, 0xff, 0xff, 0,    0,    0,    0,    0xff, 0xff, 0xff,
                         0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17,
                         0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51};
    u32 borrow = 0, nonzero = 0;
    for (u32 i = 32; i != 0;) {
        --i;
        const u32 value = scalar.data[i];
        nonzero |= value;
        borrow = (value - order[i] - borrow) >> 31;
    }
    return nonzero && borrow;
}
// Shape only: the system crypto library validates curve membership and coordinate bounds on import.
bool public_valid(ConstSlice<u8> point) noexcept {
    if (point.count != 65 || !byte_range_valid(point) || point.data[0] != 4)
        return false;
    u32 nonzero = 0;
    for (u32 i = 1; i != 65; ++i)
        nonzero |= point.data[i];
    return nonzero != 0;
}
P256Result destination(const P256Key& key, Slice<u8> output, u32 needed) noexcept {
    if (!key.handle)
        return {0, 0, P256Error::not_initialized};
    if (!byte_range_valid({output.data, output.count}))
        return {0, 0, P256Error::invalid_input};
    if (output.count < needed)
        return {0, 0, P256Error::output_capacity};
    if (byte_ranges_overlap({output.data, needed}, bytes(key)))
        return {0, 0, P256Error::invalid_input};
    return {};
}

} // namespace
} // namespace p256_internal
} // namespace maicord
