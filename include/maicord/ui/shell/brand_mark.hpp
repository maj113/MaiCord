#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// MaiCord's mark: an "M" of two posts and a centre valley with round caps, in
// fractions of its square tile. The tray and window icons rasterize it on the
// brand tile (tools/generate_app_icon.py mirrors these numbers); the server
// rail's Home tile strokes it in the tile's ink.
inline constexpr float kBrandMarkPoints[5][2] = {{.28f, .71f}, {.28f, .31f}, {.5f, .55f}, {.72f, .31f}, {.72f, .71f}};
inline constexpr float kBrandMarkStroke = .144f;
// The brand tile's gradient, 0xRRGGBB from its top edge to its bottom edge (the executable's icon, the banner).
inline constexpr u32 kBrandMarkTop = 0x8B5CF6, kBrandMarkBottom = 0xC026D3;

} // namespace maicord
