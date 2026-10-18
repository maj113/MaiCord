#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

inline constexpr u32 kAppIconLargestExtent = 64;

// The module's icon (group 1) at `extent` pixels as premultiplied BGRA rows:
// the smallest stored PNG image at least that large, box-filtered down.
// False when the resource is missing, not 8-bit RGBA or extent is above
// kAppIconLargestExtent.
bool app_icon_decode(HMODULE module, u32 extent, u32* pixels) noexcept;

} // namespace maicord
