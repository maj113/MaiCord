#include "maicord/render/media/video_frame_gpu.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/gpu/video_planes.hpp"
#include "maicord/media/video/yuv_convert_common.hpp"
#include "gpu/gles/internal.hpp"
#include "gpu/gles/video_planes_internal.hpp"

// The OpenGL ES conversion of a decoder's picture: the luma and chroma planes a decoder published (planes of
// maicord/gpu/video_planes.hpp, from a dma-buf or from CPU memory) are made GL textures here, on the renderer's
// thread, and the same compute shader as Direct3D 11's turns them into RGB.

namespace maicord {
namespace {

struct VideoConstants {
    u32 crop_x, crop_y, width, height, x, y;
    YuvRgbCoefficients color;
    u32 step, visible_width, visible_height, rotation;
};
static_assert(sizeof(VideoConstants) == 64);

GpuStatus ensure_resources(VideoFrameGpu* gpu, GpuDevice* device) noexcept {
    GpuStatus status = kGpuOk;
    if (!gpu->shader)
        status = gpu_compute_shader_create(device, ShaderId::video_frame, &gpu->shader);
    if (gpu_ok(status) && !gpu->constants)
        status = gpu_buffer_create(device, {sizeof(VideoConstants), GpuUsage::dynamic, gpu_bind_constant, false}, nullptr,
                                   &gpu->constants);
    return status;
}

} // namespace

GpuStatus video_frame_gpu_present(VideoFrameGpu* gpu, GpuDevice* device, GpuContext1* context,
                                  const GpuVideoFrame& frame, GpuUav* destination, u32 x, u32 y, u32 step) noexcept {
    // The planes are the decoder's: a luma plane (the frame's texture) and a chroma plane of half its extent.
    if (!gpu || !device || !context || !frame.texture || !frame.luma || !frame.chroma || !destination ||
        !destination->texture || frame.rotation > 3 || frame.width == 0 || frame.height == 0 || step == 0 ||
        step > kGpuVideoReductionMaximum)
        return kGpuInvalidArgument;
    const u32 width = gpu_video_frame_reduced(frame.width, step);
    const u32 height = gpu_video_frame_reduced(frame.height, step);
    const u32 output_width = gpu_video_frame_output_width(frame, step);
    const u32 output_height = gpu_video_frame_output_height(frame, step);
    const GpuTexture* luma = frame.luma->texture;
    const GpuTexture* chroma = frame.chroma->texture;
    // 8-bit planes (NV12) or 16-bit ones (P010: the sample is the 10-bit value in the top bits, which the shader's
    // 8-bit reading of a normalised sample turns into the nearest 8-bit level).
    if (frame.luma->kind != GpuViewKind::texture || frame.chroma->kind != GpuViewKind::texture || !luma || !chroma ||
        luma != frame.texture || !gpu_video_plane_owns(frame.texture) || !gpu_video_plane_owns(chroma) ||
        !((luma->format == GpuFormat::r8 && chroma->format == GpuFormat::rg8) ||
          (luma->format == GpuFormat::r16 && chroma->format == GpuFormat::rg16)) ||
        chroma->width != luma->width / 2 || chroma->height != luma->height / 2 ||
        !gpu_video_frame_geometry_valid(frame, luma->width, luma->height))
        return kGpuInvalidArgument;
    const GpuTexture* target = destination->texture;
    if (target->format != GpuFormat::rgba8 || x > target->width || output_width > target->width - x ||
        y > target->height || output_height > target->height - y)
        return kGpuInvalidArgument;
    GpuStatus status = ensure_resources(gpu, device);
    if (!gpu_ok(status))
        return status;
    gpu_video_planes_reap(device);
    if (!gles::video_plane_realize(frame.luma) || !gles::video_plane_realize(frame.chroma))
        return kGpuFail;
    void* mapped = gpu_map(context, gpu->constants, GpuMap::discard, &status);
    if (!mapped)
        return gpu_ok(status) ? kGpuFail : status;
    const VideoConstants constants{frame.crop_x, frame.crop_y, width, height, x, y,
                                   yuv_rgb_coefficients(frame.bt709, frame.full_range), step, frame.width, frame.height,
                                   frame.rotation};
    copy_bytes(mapped, &constants, sizeof(constants));
    gpu_unmap(context, gpu->constants);
    GpuSrv* inputs[]{frame.luma, frame.chroma};
    // Leave every compute binding clean for the next render pass.
    GpuSrv* empty[3]{};
    gpu_set_srvs(context, GpuStage::pixel, 0, 3, empty);
    gpu_set_render_target(context, nullptr);
    gpu_set_shader(context, gpu->shader);
    gpu_set_constants(context, GpuStage::compute, 0, 1, &gpu->constants);
    gpu_set_srvs(context, GpuStage::compute, 0, 2, inputs);
    gpu_set_uavs(context, 0, 1, &destination);
    gpu_dispatch(context, (output_width + 7u) / 8u, (output_height + 7u) / 8u, 1);
    gpu_set_srvs(context, GpuStage::compute, 0, 2, empty);
    GpuUav* no_uav = nullptr;
    gpu_set_uavs(context, 0, 1, &no_uav);
    GpuBuffer* no_buffer = nullptr;
    gpu_set_constants(context, GpuStage::compute, 0, 1, &no_buffer);
    gpu_set_shader(context, static_cast<GpuComputeShader*>(nullptr));
    ++gpu->frames;
    gpu->converted_pixels += static_cast<u64>(width) * height;
    return kGpuOk;
}

void video_frame_gpu_release(VideoFrameGpu* gpu) noexcept {
    if (!gpu)
        return;
    gpu_release(gpu->shader);
    gpu_release(gpu->constants);
    reset_value<VideoFrameGpu>(gpu);
}

} // namespace maicord
