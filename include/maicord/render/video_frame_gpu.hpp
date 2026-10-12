#pragma once

#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/core/win32_types.hpp"

struct ID3D11Buffer;
struct ID3D11ComputeShader;
struct ID3D11Device;
struct ID3D11DeviceContext1;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;
struct ID3D11Multithread;

namespace maicord {

// Source-owned NV12 plane views bypass the copy. The renderer retains no
// source texture or view. Decoder arrays and absent/invalid plane views use
// one grow-only NV12 scratch surface shared by all presented videos. Both
// paths enqueue conversion before the caller releases its frame lease.
struct VideoFrameGpu {
    ID3D11Texture2D* nv12;
    ID3D11ShaderResourceView* luma;
    ID3D11ShaderResourceView* chroma;
    ID3D11ComputeShader* shader;
    ID3D11Buffer* constants;
    ID3D11Multithread* multithread;
    u64 copied_bytes;
    u64 converted_pixels;
    u32 width;
    u32 height;
    u32 frames;
    u32 allocations;
};

// step > 1 box-averages step x step source pixels into each destination
// pixel (gpu_video_frame_reduction); the destination then holds the reduced,
// rotated extent gpu_video_frame_output_width/height(frame, step).
HRESULT video_frame_gpu_present(VideoFrameGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, const GpuVideoFrame& frame,
    ID3D11UnorderedAccessView* destination, u32 x, u32 y, u32 step = 1) noexcept;
// Timing variant of video_frame_gpu_present; the plain version has no timing calls.
// ticks[] hold the time spent in validation, resources, lock/constants, copy,
// binding, dispatch and cleanup, in that order.
struct VideoFrameGpuTrace { i64 ticks[7]; };
HRESULT video_frame_gpu_present_profiled(VideoFrameGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, const GpuVideoFrame& frame,
    ID3D11UnorderedAccessView* destination, u32 x, u32 y, VideoFrameGpuTrace* trace) noexcept;
void video_frame_gpu_release(VideoFrameGpu* gpu) noexcept;

static_assert(sizeof(VideoFrameGpu) == 80);

} // namespace maicord
