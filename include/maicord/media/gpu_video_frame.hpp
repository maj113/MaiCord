#pragma once

#include "maicord/core/base.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace maicord {

// Borrowed decoder output, valid until the next read/release. The renderer
// must enqueue its conversion before it returns the media-lane lease.
// Both producers use the same multithread-protected D3D11 immediate context,
// so GPU command ordering protects the pixels without a CPU wait or readback.
struct GpuVideoFrame {
    ID3D11Texture2D* texture;
    u32 subresource;
    u32 crop_x;
    u32 crop_y;
    u32 width;
    u32 height;
    bool bt709;
    bool full_range;
    // Clockwise quarter turns applied on conversion (container display
    // matrix); crop and width/height stay in decoded, unrotated pixels.
    u8 rotation;
    u8 reserved;
    // Borrowed R8/R8G8 plane views the renderer converts from: Texture2DArray
    // views of exactly `subresource`, valid together for the frame's lifetime.
    ID3D11ShaderResourceView* luma = nullptr;
    ID3D11ShaderResourceView* chroma = nullptr;
};

static_assert(sizeof(GpuVideoFrame) == 48);

// Live streams reach 4K (3840x2160, 4096x2160); attachment decoders keep
// their own smaller limits. Crop arithmetic stays checked before subtraction
// or GPU dispatch.
inline constexpr u64 kGpuVideoVisiblePixels = u64{4096} * 2304;
// Coded surfaces may add macroblock/driver padding below the visible area.
inline constexpr u64 kGpuVideoCodedPixels = u64{4096} * 2368;
constexpr bool gpu_video_frame_visible_valid(u32 width, u32 height) noexcept {
    return width && height && width <= 4096 && height <= 4096 && u64{width} * height <= kGpuVideoVisiblePixels;
}
// Decoder slices a live stream keeps beyond its DPB: the newest presented
// picture and one held by a reader (zero-copy presentation).
inline constexpr u8 kVideoStreamPresentationSurfaces = 2;
// Composer video posters are reduced on the GPU to the local thumbnail
// extent (kLocalPreviewThumbnailExtent) with an integer box step.
inline constexpr u32 kGpuVideoPosterExtent = 400;
inline constexpr u32 kGpuVideoReductionMaximum = 16;
// Smallest integer step that fits the longer side into extent (1: native).
constexpr u32 gpu_video_frame_reduction(u32 width, u32 height, u32 extent) noexcept {
    const u32 longest = width > height ? width : height;
    const u32 step = extent && longest > extent ? (longest + extent - 1) / extent : 1;
    return step > kGpuVideoReductionMaximum ? kGpuVideoReductionMaximum : step;
}
constexpr u32 gpu_video_frame_reduced(u32 extent, u32 step) noexcept {
    return step ? (extent + step - 1) / step : 0;
}
// A quarter or three-quarter turn swaps the converted picture's axes.
constexpr bool gpu_video_frame_transposed(const GpuVideoFrame& frame) noexcept {
    return (frame.rotation & 1u) != 0;
}
// Converted (displayed) extent of a step-reduced frame.
constexpr u32 gpu_video_frame_output_width(const GpuVideoFrame& frame, u32 step) noexcept {
    return gpu_video_frame_reduced(gpu_video_frame_transposed(frame) ? frame.height : frame.width, step);
}
constexpr u32 gpu_video_frame_output_height(const GpuVideoFrame& frame, u32 step) noexcept {
    return gpu_video_frame_reduced(gpu_video_frame_transposed(frame) ? frame.width : frame.height, step);
}

constexpr bool gpu_video_frame_geometry_valid(const GpuVideoFrame& frame, u32 coded_width,
                                              u32 coded_height) noexcept {
    return gpu_video_frame_visible_valid(frame.width, frame.height) && coded_width && coded_height &&
           coded_width <= 4096 && coded_height <= 4096 &&
           u64{coded_width} * coded_height <= kGpuVideoCodedPixels &&
           ((coded_width | coded_height | frame.crop_x | frame.crop_y) & 1u) == 0 &&
           frame.crop_x <= coded_width && frame.width <= coded_width - frame.crop_x &&
           frame.crop_y <= coded_height && frame.height <= coded_height - frame.crop_y;
}

} // namespace maicord
