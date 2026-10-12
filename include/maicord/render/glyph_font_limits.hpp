#pragma once
#include "maicord/core/base.hpp"
namespace maicord {
inline constexpr u32 glyph_font_initial_capacity=32;
inline constexpr u32 glyph_font_maximum_capacity=256;
// Atlas bit 63 distinguishes color keys from monochrome keys.
static_assert(glyph_font_maximum_capacity<0x8000);
}
