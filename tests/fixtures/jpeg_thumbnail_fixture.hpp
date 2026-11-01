#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Small authored JPEGs for the CPU thumbnail conversion (Pillow/libjpeg,
// quality 70-75): 4:2:2 colour at 17 x 11, grayscale at 13 x 9, and
// progressive 4:2:0 colour at 10 x 21. The 64-bit FNV-1a hashes of the RGBA
// rows they decode to are frozen from the decoder as it stood before the
// vectorised conversion, at full detail (extent 1000) and, for the first, at
// one sample per block (extent 1).
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

inline constexpr u64 kJpeg422OddRgbaFnv1a = 0xECCFEFCBF2287E70ull;
inline constexpr u64 kJpeg422OddDcRgbaFnv1a = 0xDD306BF5D4CAE542ull;
inline constexpr u64 kJpegGrayOddRgbaFnv1a = 0x5DA325A6D2BC2A47ull;
inline constexpr u64 kJpegProgressive420OddRgbaFnv1a = 0x8CFDA61CF811FB94ull;
inline constexpr u64 kJpeg420ReferenceThumbnailFnv1a = 0x18A25A3004373DB2ull;
inline constexpr u64 kJpegCmykAdobeThumbnailFnv1a = 0xAA42C368D7EA8652ull;
inline constexpr u64 kJpegYcckProgressiveThumbnailFnv1a = 0x44185FCE09FA930Dull;

} // namespace maicord
