#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Small authored JPEGs for the CPU thumbnail conversion (Pillow/libjpeg,
// quality 70-75): 4:2:2 colour at 17 x 11, grayscale at 13 x 9, and
// progressive 4:2:0 colour at 10 x 21. The 64-bit FNV-1a hashes of the RGBA
// rows they decode to are frozen from the integer IDCT and colour conversion,
// at full detail (extent 1000) and, for the first, at one sample per block
// (extent 1).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kJpeg422Odd[]{
#embed "jpeg_thumbnail_fixture/k422_17x11.bin"
};
inline constexpr u8 kJpegGrayOdd[]{
#embed "jpeg_thumbnail_fixture/kGray13x9.bin"
};
inline constexpr u8 kJpegProgressive420Odd[]{
#embed "jpeg_thumbnail_fixture/k420Progressive10x21.bin"
};
#pragma clang diagnostic pop

inline constexpr u64 kJpeg422OddRgbaFnv1a = 0x621ADF169A5315F7ull;
inline constexpr u64 kJpeg422OddDcRgbaFnv1a = 0xDD306BF5D4CAE542ull;
inline constexpr u64 kJpegGrayOddRgbaFnv1a = 0xCD4EA4D81ECDA900ull;
inline constexpr u64 kJpegProgressive420OddRgbaFnv1a = 0x2D2950001119B2F6ull;
inline constexpr u64 kJpeg420ReferenceThumbnailFnv1a = 0x0470717054F52585ull;
inline constexpr u64 kJpegCmykAdobeThumbnailFnv1a = 0x89187B92D3CD6954ull;
inline constexpr u64 kJpegYcckProgressiveThumbnailFnv1a = 0xA38F252F3007746Aull;

} // namespace maicord
