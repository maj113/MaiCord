#include "maicord/core/byte_order.hpp"

namespace maicord {

u32 byte_load_le24(const u8* input) noexcept {
    return static_cast<u32>(input[0]) | static_cast<u32>(input[1]) << 8u |
           static_cast<u32>(input[2]) << 16u;
}

} // namespace maicord
