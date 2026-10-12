#pragma once
#include "maicord/render/d3d11_renderer.hpp"
namespace maicord {
// Injected UI states exercise native controls and retention. This fixture does
// not decode video or prove live playback. At most one compact capture.
bool video_scene_ui_probe(D3D11Renderer&,
    bool (*capture)(const wchar_t*) noexcept = nullptr) noexcept;
}
