#pragma once
#include "maicord/core/slice.hpp"
namespace maicord {
inline u32 png_crc32_reference(const u8* bytes, u32 count) noexcept {
    constexpr u32 table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    u32 crc = 0xFFFFFFFFu;
    for (u32 index = 0; index < count; ++index) {
        crc = crc >> 4 ^ table[(crc ^ bytes[index]) & 0xFu];
        crc = crc >> 4 ^ table[(crc ^ (bytes[index] >> 4)) & 0xFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

}
