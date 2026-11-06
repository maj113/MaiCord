#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Independent RFC 3533 CRC (bitwise, polynomial 0x04c11db7, no reflection) of an Ogg page; its own checksum field
// (bytes 22 to 25) counts as zero.
inline u32 ogg_page_crc(const u8* page, u32 bytes) noexcept {
    u32 crc = 0;
    for (u32 i = 0; i < bytes; ++i) {
        crc ^= u32{i >= 22 && i < 26 ? u8{0} : page[i]} << 24;
        for (u32 bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0u);
    }
    return crc;
}

// Stores the page's checksum in its checksum field.
inline void ogg_page_seal(u8* page, u32 bytes) noexcept {
    const u32 crc = ogg_page_crc(page, bytes);
    for (u32 i = 0; i < 4; ++i)
        page[22 + i] = static_cast<u8>(crc >> (i * 8));
}

// A one-packet page (stream serial 123, the packet at most 255 bytes) with its checksum; returns its bytes.
inline u32 ogg_opus_page(u8* output, u32 sequence, u8 flags, u64 granule, const u8* packet, u8 count) noexcept {
    const u32 bytes = 28u + count;
    for (u32 i = 0; i < bytes; ++i)
        output[i] = 0;
    const char capture[] = "OggS";
    for (u32 i = 0; i < 4; ++i)
        output[i] = static_cast<u8>(capture[i]);
    output[5] = flags;
    for (u32 i = 0; i < 8; ++i)
        output[6 + i] = static_cast<u8>(granule >> (i * 8));
    output[14] = 123;
    for (u32 i = 0; i < 4; ++i)
        output[18 + i] = static_cast<u8>(sequence >> (i * 8));
    output[26] = 1;
    output[27] = count;
    for (u32 i = 0; i < count; ++i)
        output[28 + i] = packet[i];
    ogg_page_seal(output, bytes);
    return bytes;
}

} // namespace maicord
