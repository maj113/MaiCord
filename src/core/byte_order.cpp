#include "maicord/core/byte_order.hpp"

namespace maicord {

u16 byte_load_le16(const u8* input) noexcept {
    return static_cast<u16>(input[0]) | static_cast<u16>(input[1]) << 8u;
}
u32 byte_load_le24(const u8* input) noexcept {
    return static_cast<u32>(input[0]) | static_cast<u32>(input[1]) << 8u |
           static_cast<u32>(input[2]) << 16u;
}
u32 byte_load_le32(const u8* input) noexcept {
    return static_cast<u32>(input[0]) | static_cast<u32>(input[1]) << 8u |
           static_cast<u32>(input[2]) << 16u | static_cast<u32>(input[3]) << 24u;
}
u64 byte_load_le64(const u8* input) noexcept {
    return static_cast<u64>(input[0]) | static_cast<u64>(input[1]) << 8u |
           static_cast<u64>(input[2]) << 16u | static_cast<u64>(input[3]) << 24u |
           static_cast<u64>(input[4]) << 32u | static_cast<u64>(input[5]) << 40u |
           static_cast<u64>(input[6]) << 48u | static_cast<u64>(input[7]) << 56u;
}

void byte_store_le32(u8* output, u32 value) noexcept {
    output[0] = static_cast<u8>(value);
    output[1] = static_cast<u8>(value >> 8u);
    output[2] = static_cast<u8>(value >> 16u);
    output[3] = static_cast<u8>(value >> 24u);
}

void byte_store_le64(u8* output, u64 value) noexcept {
    byte_store_le32(output, static_cast<u32>(value));
    byte_store_le32(output + 4, static_cast<u32>(value >> 32u));
}

void byte_store_be32(u8* output, u32 value) noexcept {
    output[0] = static_cast<u8>(value >> 24u);
    output[1] = static_cast<u8>(value >> 16u);
    output[2] = static_cast<u8>(value >> 8u);
    output[3] = static_cast<u8>(value);
}

} // namespace maicord
