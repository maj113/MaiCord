#pragma once

#include "maicord/render/d3d11_renderer.hpp"

namespace maicord {

bool d3d11_renderer_draw_stream(D3D11Renderer* renderer, const Theme& theme, bool upload_paint,
                                bool upload_media, const RECT* damage,
                                u64 effect_time_ms = ~0ull) noexcept;

} // namespace maicord
