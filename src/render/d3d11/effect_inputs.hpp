#pragma once
#include "maicord/gpu/api.hpp"
#include "maicord/render/d3d11/effect_renderer.hpp"

namespace maicord {
struct D3D11Renderer;

// Builds the 16x16 glyph grid (see effect_glyphs.cpp); the caller owns the returned view.
bool effect_glyph_grid_build(D3D11Renderer* renderer, GpuSrv** view) noexcept;

// The built-in textures a package declares (t0..t6: noise, glyph grid, blue noise, TEXTURE0..3), as
// immutable shader views; an undeclared input stays null. The glyph grid is built by the renderer (it needs
// the text system) and arrives in `glyph`, which is passed through. On failure nothing is left in `views`.
GpuStatus effect_inputs_create(GpuDevice* device, const BackgroundEffectSpec& spec,
                               GpuSrv* glyph,
                               GpuSrv* (&views)[kD3D11EffectInputs]) noexcept;

} // namespace maicord
