#pragma once
#include "maicord/render/d3d11_renderer.hpp"

namespace maicord {
bool video_retention_probe(D3D11Renderer& renderer, MediaAssetKey key,
                           u16 width, u16 height) noexcept;
}
