#pragma once
#include "maicord/render/d3d11_renderer.hpp"

namespace maicord {
// Toasts over the shell and over an open settings page, through the real
// layout, text and draw path; captures each frame when asked.
bool toast_ui_probe(D3D11Renderer&, bool (*capture)(const wchar_t*) noexcept) noexcept;
} // namespace maicord
