#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
// Caller supplies four writable bytes. Canonical MLS <V> vector length;
// zero return means out of range and leaves output unchanged.
inline u32 mls_vector_length(u32 value, u8* output) noexcept {
    if (value > 0x3fffffff)
        return 0;
    const u32 count = value < 64 ? 1u : value < 16384 ? 2u : 4u;
    for (u32 i = 0; i != count; ++i)
        output[count - i - 1] = static_cast<u8>(value >> (i * 8));
    output[0] |= count == 1 ? 0 : count == 2 ? 0x40 : 0x80;
    return count;
}
} // namespace maicord
