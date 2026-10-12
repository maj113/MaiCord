#pragma once

#include "maicord/render/d3d11_renderer.hpp"

namespace maicord {
// Uses the existing native device, with bounded in-memory readback only.
// No decoder, network, session, capture file or operating-system input proof.
bool audio_scene_ui_probe(D3D11Renderer&) noexcept;
} // namespace maicord
