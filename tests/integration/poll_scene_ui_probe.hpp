#pragma once
#include "maicord/render/d3d11_renderer.hpp"
namespace maicord {
// Real native renderer, with bounded local model and pure input authority.
// No application transport or live poll requests are exercised; capture writes
// the ballot, results, ended and voter frames when the probe captures.
using PollProbeCapture = bool (*)(const wchar_t*, const Theme&) noexcept;
bool poll_scene_ui_probe(D3D11Renderer&, PollProbeCapture capture) noexcept;
} // namespace maicord
