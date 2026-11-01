#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
struct HevcGpuSurfaceExtent {
    u32 width{}, height{};
};
// DXVA HEVC recommends surfaces aligned to 64 pixels. The coded SPS dimensions
// and its conformance crop remain separate from this allocation extent.
constexpr HevcGpuSurfaceExtent hevc_gpu_surface_extent(u32 width, u32 height) noexcept {
    if (!width || !height || width > 4096 || height > 4096 || u64{width} * height > 2048u * 2048u)
        return {};
    const u32 aligned_width = (width + 63u) & ~63u;
    const u32 aligned_height = (height + 63u) & ~63u;
    if (u64{aligned_width} * aligned_height > 2048u * 2048u)
        return {};
    return {aligned_width, aligned_height};
}
// Keep a zero-byte Annex B tail even when the payload ends on a 128-byte
// boundary. The driver-reported capacity is checked before writing it.
constexpr u64 hevc_gpu_bitstream_extent(u32 bytes) noexcept {
    return (u64{bytes} + 128u) & ~u64{127};
}
// Once per picture: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// DXVA carries the HEVC ScalingList syntax sequence, which is diagonal scan
// order. Parameter parsing keeps raster matrices for defaults and prediction.
template <u32 Side>
constexpr void hevc_gpu_scaling_diagonal(const u8 (&raster)[Side * Side],
                                         u8 (&diagonal)[Side * Side]) noexcept {
    static_assert(Side == 4 || Side == 8);
    u32 position{};
    for (u32 sum = 0; sum < 2 * Side - 1; ++sum)
        for (u32 x = 0; x < Side; ++x)
            if (sum >= x && sum - x < Side)
                diagonal[position++] = raster[(sum - x) * Side + x];
}
#pragma clang attribute pop
} // namespace maicord
