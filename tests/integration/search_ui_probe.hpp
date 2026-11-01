#pragma once
#include "maicord/render/d3d11/renderer.hpp"
namespace maicord {
bool search_ui_probe(D3D11Renderer&, bool (*capture)(const wchar_t*) noexcept) noexcept;
}
