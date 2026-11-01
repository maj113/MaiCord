#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Authored 16x16 RGB pattern, encoded once as baseline sequential JFIF with
// 4:2:0 sampling (Pillow/libjpeg, quality 88, optimized Huffman tables).
// The bytes are a test oracle only; no encoder/decoder dependency is shipped.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kJpeg420Reference[]{
#embed "jpeg_reference_fixture/kJpeg420Reference.bin"
};

inline constexpr u8 kJpeg420ReferenceRgb[]{
#embed "jpeg_reference_fixture/kJpeg420ReferenceRgb.bin"
};
#pragma clang diagnostic pop

} // namespace maicord
