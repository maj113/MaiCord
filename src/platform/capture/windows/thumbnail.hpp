#pragma once

#include "maicord/gpu/gpu.hpp"

namespace maicord {

// The thumbnail step of capture_thumbnail (Windows): copies the top-left width x height of the BGRA `source` into a
// mipmapped texture, reduces it with GenerateMips to the first level that fits max_width x max_height
// (capture_thumbnail_level) and reads that level back as opaque RGBA rows of *out_width * 4 bytes. Uses the device's
// immediate context.
bool capture_reduce_to_rgba(GpuDevice* device, GpuTexture* source, u32 width, u32 height, u32 max_width,
                            u32 max_height, u8* rgba, u32* out_width, u32* out_height) noexcept;

} // namespace maicord
