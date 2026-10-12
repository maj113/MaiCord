#pragma once
#include "maicord/render/d3d11_renderer.hpp"

namespace maicord {
bool tooltip_ui_probe(D3D11Renderer&, bool (*capture)(const wchar_t*) noexcept) noexcept;
}
