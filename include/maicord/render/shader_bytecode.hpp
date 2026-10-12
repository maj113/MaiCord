#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

struct ID3D11ComputeShader;
struct ID3D11Device;
struct ID3D11InputLayout;
struct ID3D11PixelShader;
struct ID3D11VertexShader;
struct D3D11_INPUT_ELEMENT_DESC;

namespace maicord {

// Shaders use deflate streams with an optional exact sparse variant patch.
// Each call reconstructs one shader into
// stack scratch right before the D3D11 Create* call, which copies the bytecode,
// so nothing stays resident. The first-frame UI/glyph/media set adds well under
// 0.1 ms of inflation to startup. Order matches the pack_shader_bytecode.py
// arguments in CMakeLists.txt (checked at compile time).
enum class ShaderId : u8 {
    ui_vertex, ui_pixel, glyph_vertex, glyph_pixel, media_vertex, media_pixel, glyph_compute,
    colr_compute, colr_wide_compute, colr_layers_compute, png_compute,
    vp8_reconstruct, vp8_idct, vp8_filter, vp8_atlas, jpeg_idct, jpeg_atlas, video_frame,
    fx_vertex, fx_builtin,
    count
};

HRESULT shader_create_compute(ID3D11Device* device, ShaderId id, ID3D11ComputeShader** shader) noexcept;
HRESULT shader_create_pixel(ID3D11Device* device, ShaderId id, ID3D11PixelShader** shader) noexcept;
// Creates the vertex shader, then its input layout from the same bytecode.
// *layout_failed tells which of the two calls returned the failure.
// A null layout creates only the shader (vertex-id-only programs).
HRESULT shader_create_vertex(ID3D11Device* device, ShaderId id, ConstSlice<D3D11_INPUT_ELEMENT_DESC> elements,
                             ID3D11VertexShader** shader, ID3D11InputLayout** layout,
                             bool* layout_failed) noexcept;

} // namespace maicord
