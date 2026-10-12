#pragma once
#include "maicord/render/d3d11_renderer.hpp"
namespace maicord {
// Real native renderer, with bounded local model and pure input authority.
// No application transport, capture files, or live poll requests are exercised.
bool poll_scene_ui_probe(D3D11Renderer&) noexcept;
} // namespace maicord
