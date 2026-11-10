#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// The colours that stand for an image, the way profiles take their avatar's (banner fill, Nitro theme): every tenth
// pixel (in row order) that is neither translucent (alpha below 125) nor near-white (every channel above 250) goes
// into a median cut quantization at 5 bits per channel, split into boxes of pixels first by population (to 4 boxes)
// and then by population times volume (to 5). The palette is the boxes' mean colours, largest population times
// volume first; black alone when no pixel is left.
// `rgba` holds width * height straight RGBA8 pixels, row after row; `histogram` is scratch of at least
// kImageDominantColorHistogramEntries entries; `colors` takes kImagePaletteColors. Returns how many colours it wrote
// (0 for bad input), each 0xRRGGBB.
inline constexpr u32 kImageDominantColorHistogramEntries = 1u << 15;
inline constexpr u32 kImagePaletteColors = 5;
u32 image_palette(ConstSlice<u8> rgba, u32 width, u32 height, Slice<u32> histogram, u32* colors) noexcept;
// The colour that stands beside `rgb` when a palette has only that one: its hue a quarter turn back, saturation
// raised to at least 30 % and lightness kept within 20-80 % (whole-degree hue, one-decimal percentages).
u32 image_palette_companion(u32 rgb) noexcept;

} // namespace maicord
