#pragma once

#include "maicord/media/video/gpu_video_frame.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {

// Converts from the frame's source-owned NV12 plane views (one-slice array
// views of its subresource); a frame without valid views is E_INVALIDARG. The
// renderer retains no source texture or view and enqueues the conversion
// before the caller releases its frame lease.
struct VideoFrameGpu {
    GpuComputeShader* shader;
    GpuBuffer* constants;
    void* context_lock; // the backend's lock on the shared context (Direct3D 11: its ID3D11Multithread)
    u64 converted_pixels;
    u32 frames;
};

// step > 1 box-averages step x step source pixels into each destination
// pixel (gpu_video_frame_reduction); the destination then holds the reduced,
// rotated extent gpu_video_frame_output_width/height(frame, step).
GpuStatus video_frame_gpu_present(VideoFrameGpu* gpu, GpuDevice* device,
    GpuContext1* context, const GpuVideoFrame& frame,
    GpuUav* destination, u32 x, u32 y, u32 step = 1) noexcept;
void video_frame_gpu_release(VideoFrameGpu* gpu) noexcept;

static_assert(sizeof(VideoFrameGpu) == 40);

} // namespace maicord
