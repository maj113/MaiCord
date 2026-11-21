#pragma once

#include "maicord/render/media/video_frame_gpu.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/media/video/yuv_convert_common.hpp"

// The part of video_frame_gpu_present that Direct3D 11 and OpenGL ES share: the constants of video_frame.hlsl and
// its compute pass. Each backend validates the frame and the destination first.

namespace maicord {

struct VideoConstants {
    u32 crop_x, crop_y, width, height, x, y;
    YuvRgbCoefficients color;
    u32 step, visible_width, visible_height, rotation;
    u32 lanes, unused[3];
};
static_assert(sizeof(VideoConstants) == 80);

// Not inline, as each backend's own copy was: the Linux build inlines only functions with the inline hint.
static GpuStatus video_frame_resources(VideoFrameGpu* gpu, GpuDevice* device) noexcept {
    GpuStatus status = kGpuOk;
    if (!gpu->shader)
        status = gpu_compute_shader_create(device, ShaderId::video_frame, &gpu->shader);
    if (gpu_ok(status) && !gpu->constants)
        status = gpu_buffer_create(device, {sizeof(VideoConstants), GpuUsage::dynamic, gpu_bind_constant, false}, nullptr,
                                   &gpu->constants);
    return status;
}

// The caller holds the context and passes the status of video_frame_resources. width and height are the reduced
// source extent, output_* the rotated one.
__forceinline GpuStatus video_frame_convert(VideoFrameGpu* gpu, GpuContext1* context, const GpuVideoFrame& frame,
                                            GpuUav* const& destination, u32 x, u32 y, u32 step, u32 width, u32 height,
                                            u32 output_width, u32 output_height, GpuStatus& status) noexcept {
    void* mapped = gpu_map(context, gpu->constants, GpuMap::discard, &status);
    if (!mapped)
        return gpu_ok(status) ? kGpuFail : status;
    const u32 lanes = video_frame_gpu_lanes(step);
    const VideoConstants constants{frame.crop_x, frame.crop_y, width, height, x, y,
                                   yuv_rgb_coefficients(frame.bt709, frame.full_range), step, frame.width, frame.height,
                                   frame.rotation, lanes, {}};
    copy_bytes(mapped, &constants, sizeof(constants));
    gpu_unmap(context, gpu->constants);
    GpuSrv* inputs[]{frame.luma, frame.chroma};
    // The atlas can still be bound by the preceding draw. Unbind it before
    // using the UAV; leave all compute bindings clean for the next render pass.
    GpuSrv* empty[3]{};
    gpu_set_srvs(context, GpuStage::pixel, 0, 3, empty);
    gpu_set_render_target(context, nullptr);
    gpu_set_shader(context, gpu->shader);
    gpu_set_constants(context, GpuStage::compute, 0, 1, &gpu->constants);
    gpu_set_srvs(context, GpuStage::compute, 0, 2, inputs);
    gpu_set_uavs(context, 0, 1, &destination);
    if (lanes > 1)
        gpu_dispatch(context, (output_width + 64u / lanes - 1u) / (64u / lanes), output_height, 1);
    else
        gpu_dispatch(context, (output_width + 7u) / 8u, (output_height + 7u) / 8u, 1);
    gpu_set_srvs(context, GpuStage::compute, 0, 2, empty);
    GpuUav* no_uav = nullptr;
    gpu_set_uavs(context, 0, 1, &no_uav);
    GpuBuffer* no_buffer = nullptr;
    gpu_set_constants(context, GpuStage::compute, 0, 1, &no_buffer);
    gpu_set_shader(context, static_cast<GpuComputeShader*>(nullptr));
    ++gpu->frames;
    return kGpuOk;
}

} // namespace maicord
