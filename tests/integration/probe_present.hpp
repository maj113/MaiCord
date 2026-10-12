#pragma once
#include "maicord/render/d3d11_renderer.hpp"

namespace maicord {
// Integration assertions cover paint, damage, swaps and interaction. Desktop
// refresh pacing is a separate concern; preserve accepted BeginFrame permits
// and restore the caller's resize policy after this unsynchronized present.
inline bool probe_present(D3D11Renderer* renderer, const Theme& theme) noexcept {
    const bool live_resize = renderer->live_resize;
    renderer->live_resize = true;
    const bool result = renderer_present(renderer, theme);
    renderer->live_resize = live_resize;
    return result;
}
} // namespace maicord
