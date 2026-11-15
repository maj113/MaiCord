#include "maicord/core/byte_order.hpp"

namespace maicord {

u32 byte_load_le24(const u8* input) noexcept {
    return static_cast<u32>(input[0]) | static_cast<u32>(input[1]) << 8u |
           static_cast<u32>(input[2]) << 16u;
}

u32 leb128_bytes(u32 value) noexcept {
    u32 bytes = 1;
    for (; value >= 0x80; value >>= 7)
        ++bytes;
    return bytes;
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
