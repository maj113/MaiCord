#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/gpu/gpu.hpp"
#include "maicord/gpu/shader_id.hpp"

struct D3D11_INPUT_ELEMENT_DESC;

namespace maicord {

// Shaders are deflate streams, optionally with an exact sparse variant patch.
// Each call inflates one shader into stack scratch right before the D3D11
// Create* call, which copies the bytecode, so nothing stays resident. Order
// must match the shader list in cmake/generated_sources.cmake (checked at
// compile time).
GpuStatus shader_create_compute(GpuDevice* device, ShaderId id, GpuComputeShader** shader) noexcept;
GpuStatus shader_create_pixel(GpuDevice* device, ShaderId id, GpuPixelShader** shader) noexcept;
// Creates the vertex shader, then its input layout from the same bytecode.
// *layout_failed tells which of the two calls returned the failure.
// A null layout creates only the shader (vertex-id-only programs).
GpuStatus shader_create_vertex(GpuDevice* device, ShaderId id, ConstSlice<D3D11_INPUT_ELEMENT_DESC> elements,
                             GpuVertexShader** shader, GpuInputLayout** layout,
                             bool* layout_failed) noexcept;

} // namespace maicord
