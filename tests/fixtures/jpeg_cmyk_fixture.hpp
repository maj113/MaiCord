#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Authored 16x16 CMYK pattern encoded once with libjpeg-turbo
// 3.2.1 commit 1157d37. The baseline fixture uses Adobe APP14
// transform 0; the progressive fixture uses transform 2 (YCCK) and
// 2x2 chroma sampling. Pillow/libjpeg RGB results are frozen as GPU
// oracles only; no encoder or decoder dependency is shipped.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
inline constexpr u8 kJpegCmykAdobeReference[]{
#embed "jpeg_cmyk_fixture/kJpegCmykAdobeReference.bin"
};

inline constexpr u8 kJpegCmykAdobeReferenceRgb[]{
#embed "jpeg_cmyk_fixture/kJpegCmykAdobeReferenceRgb.bin"
};

inline constexpr u8 kJpegYcckProgressiveReference[]{
#embed "jpeg_cmyk_fixture/kJpegYcckProgressiveReference.bin"
};

inline constexpr u8 kJpegYcckProgressiveReferenceRgb[]{
#embed "jpeg_cmyk_fixture/kJpegYcckProgressiveReferenceRgb.bin"
};
#pragma clang diagnostic pop

static_assert(sizeof(kJpegCmykAdobeReference) == 634);
static_assert(sizeof(kJpegCmykAdobeReferenceRgb) == 16 * 16 * 3);
static_assert(sizeof(kJpegYcckProgressiveReference) == 1'141);
static_assert(sizeof(kJpegYcckProgressiveReferenceRgb) == 16 * 16 * 3);

} // namespace maicord
