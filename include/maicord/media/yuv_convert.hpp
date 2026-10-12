#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// A null v_plane denotes interleaved NV12 chroma; otherwise the planes are
// I420. Strides describe storage, width/height describe the visible aperture.
void yuv420_to_rgba8_scalar(const u8* y_plane, const u8* u_plane,
                            const u8* v_plane, u32 y_pitch, u32 chroma_pitch,
                            u8* destination, u32 destination_pitch,
                            u32 width, u32 height, bool bt709,
                            bool full_range) noexcept;

void nv12_to_rgba8_avx2(const u8* y_plane, const u8* uv_plane,
                        u32 source_pitch, u8* destination,
                        u32 destination_pitch, u32 width, u32 height,
                        bool bt709, bool full_range) noexcept;

} // namespace maicord
