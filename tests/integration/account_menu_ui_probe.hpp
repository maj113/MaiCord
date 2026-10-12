#pragma once
#include "maicord/render/d3d11_renderer.hpp"
namespace maicord {
bool account_menu_ui_probe(D3D11Renderer&, bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept;
}
