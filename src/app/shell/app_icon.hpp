#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

inline constexpr u32 kAppIconLargestExtent = 64;

// The program's icon at `extent` pixels as premultiplied BGRA rows (window_app_icon_png): the smallest stored PNG
// image at least that large, box-filtered down. Accepts PNGs with 8-bit palette indices and palette alpha.
// False for a missing or unsupported icon, null pixels or an invalid extent.
bool app_icon_decode(u32 extent, u32* pixels) noexcept;

} // namespace maicord
