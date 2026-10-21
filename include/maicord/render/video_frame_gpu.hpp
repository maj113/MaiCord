#pragma once

#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/core/win32_types.hpp"

struct ID3D11Buffer;
struct ID3D11ComputeShader;
struct ID3D11Device;
struct ID3D11DeviceContext1;
struct ID3D11UnorderedAccessView;
struct ID3D11Multithread;

namespace maicord {

// Converts from the frame's source-owned NV12 plane views (one-slice array
// views of its subresource); a frame without valid views is E_INVALIDARG. The
// renderer retains no source texture or view and enqueues the conversion
// before the caller releases its frame lease.
struct VideoFrameGpu {
    ID3D11ComputeShader* shader;
    ID3D11Buffer* constants;
    ID3D11Multithread* multithread;
    u64 converted_pixels;
    u32 frames;
};

// step > 1 box-averages step x step source pixels into each destination
// pixel (gpu_video_frame_reduction); the destination then holds the reduced,
// rotated extent gpu_video_frame_output_width/height(frame, step).
HRESULT video_frame_gpu_present(VideoFrameGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, const GpuVideoFrame& frame,
    ID3D11UnorderedAccessView* destination, u32 x, u32 y, u32 step = 1) noexcept;
void video_frame_gpu_release(VideoFrameGpu* gpu) noexcept;

static_assert(sizeof(VideoFrameGpu) == 40);

} // namespace maicord
