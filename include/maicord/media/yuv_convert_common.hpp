#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

struct YuvRgbCoefficients {
    i32 y_scale;
    i32 y_offset;
    i32 red_cr;
    i32 green_cb;
    i32 green_cr;
    i32 blue_cb;
};

constexpr YuvRgbCoefficients yuv_rgb_coefficients(bool bt709,
                                                  bool full) noexcept {
    if (full) {
        return bt709 ? YuvRgbCoefficients{256, 0, 403, 48, 120, 475}
                     : YuvRgbCoefficients{256, 0, 359, 88, 183, 454};
    }
    return bt709 ? YuvRgbCoefficients{298, 16, 459, 55, 136, 541}
                 : YuvRgbCoefficients{298, 16, 409, 100, 208, 516};
}

} // namespace maicord
