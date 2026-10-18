#pragma once

#include "maicord/core/byte_order.hpp"

namespace maicord {

inline constexpr u8 kPngSignature[8] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
};

constexpr u32 png_chunk_type(u8 a, u8 b, u8 c, u8 d) noexcept {
    return static_cast<u32>(a) << 24 | static_cast<u32>(b) << 16 |
           static_cast<u32>(c) << 8 | d;
}

inline constexpr u32 kChunkIhdr = png_chunk_type('I', 'H', 'D', 'R');
inline constexpr u32 kChunkPlte = png_chunk_type('P', 'L', 'T', 'E');
inline constexpr u32 kChunkIdat = png_chunk_type('I', 'D', 'A', 'T');
inline constexpr u32 kChunkIend = png_chunk_type('I', 'E', 'N', 'D');
inline constexpr u32 kChunkTrns = png_chunk_type('t', 'R', 'N', 'S');
inline constexpr u32 kChunkActl = png_chunk_type('a', 'c', 'T', 'L');
inline constexpr u32 kChunkFctl = png_chunk_type('f', 'c', 'T', 'L');
inline constexpr u32 kChunkFdat = png_chunk_type('f', 'd', 'A', 'T');
inline constexpr u32 kChunkSrgb = png_chunk_type('s', 'R', 'G', 'B');
inline constexpr u32 kChunkGama = png_chunk_type('g', 'A', 'M', 'A');
inline constexpr u32 kChunkChrm = png_chunk_type('c', 'H', 'R', 'M');
inline constexpr u32 kChunkIccp = png_chunk_type('i', 'C', 'C', 'P');
inline constexpr u32 kChunkCicp = png_chunk_type('c', 'I', 'C', 'P');

constexpr bool png_chunk_type_valid(const u8* type) noexcept {
    for (u32 index = 0; index < 4; ++index) {
        if (!((type[index] >= 'A' && type[index] <= 'Z') || (type[index] >= 'a' && type[index] <= 'z'))) {
            return false;
        }
    }
    return true;
}

constexpr bool png_color_metadata_chunk(u32 type) noexcept {
    return type == kChunkSrgb || type == kChunkGama || type == kChunkChrm || type == kChunkIccp ||
           type == kChunkCicp;
}

struct PngImage;
// Channel count of a colour type; false when the bit depth is not allowed for it.
bool png_format_channels(u8 color_type, u8 bit_depth, u8* channels) noexcept;
// Store a PLTE / tRNS payload in image; false when the chunk does not fit the colour type.
bool png_apply_palette(PngImage* image, const u8* data, u32 length) noexcept;
bool png_apply_transparency(PngImage* image, const u8* data, u32 length) noexcept;

u32 png_crc32(const u8* bytes, u32 count) noexcept;
// Reconstructs one filtered scanline of count bytes in place (filter byte
// excluded). previous is the reconstructed row above, or null for the first
// row. Returns false for a filter type above 4.
bool png_unfilter_row(u8 filter, u8* bytes, const u8* previous, u32 count, u32 bpp) noexcept;

} // namespace maicord
