#pragma once
#include "maicord/render/d3d11/renderer.hpp"

namespace maicord {
bool video_display_extent_probe(D3D11Renderer& renderer) noexcept;
bool video_retention_probe(D3D11Renderer& renderer, MediaAssetKey key,
                           u16 width, u16 height) noexcept;
}
