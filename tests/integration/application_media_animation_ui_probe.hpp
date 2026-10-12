#pragma once

#include "maicord/render/d3d11_renderer.hpp"

namespace maicord {
bool application_media_animation_ui_probe(D3D11Renderer& renderer,
                                          bool (*capture)(const wchar_t*) noexcept) noexcept;
}
