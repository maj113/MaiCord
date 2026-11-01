#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

inline constexpr u32 kAppIconLargestExtent = 64;

// The module's icon (group 1) at `extent` pixels as premultiplied BGRA rows:
// the smallest stored PNG image at least that large, box-filtered down.
// Accepts PNGs with 8-bit palette indices and palette alpha.
// False for missing/unsupported resources, null pixels or an invalid extent.
bool app_icon_decode(HMODULE module, u32 extent, u32* pixels) noexcept;

} // namespace maicord
