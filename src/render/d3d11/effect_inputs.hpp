#pragma once
#include "maicord/render/d3d11/effect_renderer.hpp"

namespace maicord {
struct D3D11Renderer;

// Builds the 16x16 glyph grid (see effect_glyphs.cpp); the caller owns the returned view.
bool effect_glyph_grid_build(D3D11Renderer* renderer, ID3D11ShaderResourceView** view) noexcept;

// The built-in textures a package declares (t0..t4: noise, glyph grid, blue noise, TEXTURE0, TEXTURE1), as
// immutable shader views; an undeclared input stays null. The glyph grid is built by the renderer (it needs
// the text system) and arrives in `glyph`, which is passed through. On failure nothing is left in `views`.
HRESULT effect_inputs_create(ID3D11Device* device, const BackgroundEffectSpec& spec,
                             ID3D11ShaderResourceView* glyph,
                             ID3D11ShaderResourceView* (&views)[kD3D11EffectInputs]) noexcept;

} // namespace maicord
