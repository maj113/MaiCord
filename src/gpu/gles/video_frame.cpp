#include "../video_frame_convert.hpp"

#include "maicord/gpu/video_planes.hpp"
#include "gpu/gles/internal.hpp"
#include "gpu/gles/video_planes_internal.hpp"

// The OpenGL ES conversion of a decoder's picture: the luma and chroma planes a decoder published (planes of
// maicord/gpu/video_planes.hpp, from a dma-buf or from CPU memory) are made GL textures here, on the renderer's
// thread, and the same compute shader as Direct3D 11's turns them into RGB.

namespace maicord {

GpuStatus video_frame_gpu_present(VideoFrameGpu* gpu, GpuDevice* device, GpuContext1* context,
                                  const GpuVideoFrame& frame, GpuUav* destination, u32 x, u32 y, u32 step) noexcept {
    // The planes are the decoder's: a luma plane (the frame's texture) and a chroma plane of half its extent.
    if (!frame.texture || !frame.luma || !frame.chroma || !destination ||
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
    GpuStatus status = video_frame_resources(gpu, device);
    if (!gpu_ok(status))
        return status;
    gpu_video_planes_reap(device);
    if (!gles::video_plane_realize(frame.luma) || !gles::video_plane_realize(frame.chroma))
        return kGpuFail;
    return video_frame_convert(gpu, context, frame, destination, x, y, step, width, height, output_width,
                               output_height, status);
}

void video_frame_gpu_release(VideoFrameGpu* gpu) noexcept {
    gpu_release(gpu->shader);
    gpu_release(gpu->constants);
    reset_value<VideoFrameGpu>(gpu);
}

} // namespace maicord
