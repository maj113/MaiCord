#include "maicord/media/yuv_convert.hpp"

#include "maicord/media/yuv_convert_common.hpp"

namespace maicord {

void yuv420_to_rgba8_scalar(const u8* y_plane, const u8* u_plane,
                            const u8* v_plane, u32 y_pitch, u32 chroma_pitch,
                            u8* destination, u32 destination_pitch,
                            u32 width, u32 height, bool bt709,
                            bool full_range) noexcept {
    const YuvRgbCoefficients c = yuv_rgb_coefficients(bt709, full_range);
    const bool nv12 = v_plane == nullptr;
    for (u32 y = 0; y < height; ++y) {
        const u8* y_row = y_plane + static_cast<u64>(y) * y_pitch;
        const u8* u_row = u_plane + static_cast<u64>(y / 2u) * chroma_pitch;
        const u8* v_row = nv12 ? nullptr
                              : v_plane + static_cast<u64>(y / 2u) * chroma_pitch;
        u8* output = destination + static_cast<u64>(y) * destination_pitch;
        for (u32 x = 0; x < width; ++x) {
            const i32 cb = u_row[nv12 ? x & ~1u : x / 2u] - 128;
            const i32 cr = nv12 ? u_row[(x & ~1u) + 1u] - 128
                                : v_row[x / 2u] - 128;
            yuv_rgb_pixel(output + x * 4u, y_row[x], cb, cr, c);
        }
    }
}

} // namespace maicord
