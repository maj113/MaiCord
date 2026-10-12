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

constexpr u8 yuv_clamp_rgb(i32 value) noexcept {
    return static_cast<u8>(value < 0 ? 0 : value > 255 ? 255 : value);
}

inline void yuv_rgb_pixel(u8* output, i32 y, i32 cb, i32 cr,
                           const YuvRgbCoefficients& c) noexcept {
    const i32 luma = (y - c.y_offset) * c.y_scale;
    // Keep below-black luma signed until the final RGB clamp. Full-range
    // negative chroma uses the same rounding as the vector path.
    output[0] = yuv_clamp_rgb((luma + c.red_cr * cr + 128) >> 8);
    output[1] = yuv_clamp_rgb(
        (luma - c.green_cb * cb - c.green_cr * cr + 128) >> 8);
    output[2] = yuv_clamp_rgb((luma + c.blue_cb * cb + 128) >> 8);
    output[3] = 255;
}

} // namespace maicord
