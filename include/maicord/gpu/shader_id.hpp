#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// The built-in shaders. Each backend ships a pack of them in this order (Direct3D 11: the DXBC pack of
// cmake/generated_sources.cmake, checked at compile time in src/gpu/d3d11/shader_pack.cpp; OpenGL ES: the
// GLSL pack of tools/build_gles_shader_pack.py, checked in src/gpu/gles/shader_pack.cpp).
enum class ShaderId : u8 {
    ui_vertex, ui_pixel, glyph_vertex, glyph_pixel, media_vertex, media_pixel, glyph_compute,
    glyph_culled_compute,
    colr_compute, colr_wide_compute, colr_layers_compute, png_compute,
    vp8_reconstruct, vp8_idct, vp8_filter, vp8_atlas, jpeg_idct, jpeg_idct_rows, jpeg_atlas,
    video_frame,
    fx_vertex, fx_builtin, fx_target_vertex, fx_composite_vertex, fx_composite_pixel,
    count
};

} // namespace maicord
