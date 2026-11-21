#include "maicord/core/byte_order.hpp"

namespace maicord {

u32 byte_load_le24(const u8* input) noexcept {
    return static_cast<u32>(input[0]) | static_cast<u32>(input[1]) << 8u |
           static_cast<u32>(input[2]) << 16u;
}

// Seven bits per byte: 1 + h / 7 bytes for a highest set bit h, computed as (h * 9 + 73) / 64.
u32 leb128_bytes(u32 value) noexcept {
    return ((31 - static_cast<u32>(__builtin_clz(value | 1))) * 9 + 73) / 64;
}

u32 leb128_write(u8* output, u32 value) noexcept {
    u32 n = 0;
    do {
        output[n++] = static_cast<u8>((value & 0x7f) | (value >= 0x80 ? 0x80 : 0));
        value >>= 7;
    } while (value);
    return n;
}

} // namespace maicord
