#pragma once

namespace maicord {

// MaiCord's mark: an "M" of two posts and a centre valley with round caps, in
// fractions of its square tile. The tray and window icons rasterize it on the
// brand tile (tools/generate_app_icon.py mirrors these numbers); the server
// rail's Home tile strokes it in the tile's ink.
inline constexpr float kBrandMarkPoints[5][2] = {{.28f, .71f}, {.28f, .31f}, {.5f, .55f}, {.72f, .31f}, {.72f, .71f}};
inline constexpr float kBrandMarkStroke = .144f;

} // namespace maicord
