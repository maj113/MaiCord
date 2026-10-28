#pragma once

namespace maicord {
struct D3D11Renderer;
bool menu_row_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept;
}
