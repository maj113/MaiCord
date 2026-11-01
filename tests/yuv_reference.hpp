#pragma once
// Scalar reference for the shader's YUV to RGB conversion in the GPU tests.
#include "maicord/media/yuv_convert_common.hpp"

namespace maicord {

inline u8 yuv_reference_clamp(i32 value) noexcept {
    return static_cast<u8>(value < 0 ? 0 : value > 255 ? 255 : value);
}

// Keeps below-black luma signed until the final RGB clamp. Full-range negative
// chroma uses the same rounding as the vector path.
inline void yuv_rgb_pixel(u8* output, i32 y, i32 cb, i32 cr, const YuvRgbCoefficients& c) noexcept {
    const i32 luma = (y - c.y_offset) * c.y_scale;
    output[0] = yuv_reference_clamp((luma + c.red_cr * cr + 128) >> 8);
    output[1] = yuv_reference_clamp((luma - c.green_cb * cb - c.green_cr * cr + 128) >> 8);
    output[2] = yuv_reference_clamp((luma + c.blue_cb * cb + 128) >> 8);
    output[3] = 255;
}

} // namespace maicord
