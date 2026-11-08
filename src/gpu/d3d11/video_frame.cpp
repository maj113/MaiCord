#include "maicord/render/media/video_frame_gpu.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/media/video/yuv_convert_common.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/sys/d3d11.hpp"

// The Direct3D 11 conversion of a decoder's NV12 frame: the frame's texture, plane views and the destination's
// format are validated against the D3D11 objects themselves, and the immediate context is locked through its
// ID3D11Multithread (decode workers share it). The OpenGL ES build converts imported planes in its own backend.

namespace maicord {
namespace {

struct VideoConstants {
    u32 crop_x, crop_y, width, height, x, y;
    YuvRgbCoefficients color;
    u32 step, visible_width, visible_height, rotation;
    u32 lanes, unused[3];
};
static_assert(sizeof(VideoConstants) == 80);

GpuStatus ensure_resources(VideoFrameGpu* gpu, GpuDevice* device) noexcept {
    GpuStatus status = S_OK;
    if (!gpu->shader) {
        status = gpu_compute_shader_create(device, ShaderId::video_frame, &gpu->shader);
    }
    if (SUCCEEDED(status) && !gpu->constants) {
        status = gpu_buffer_create(device, {sizeof(VideoConstants), GpuUsage::dynamic, gpu_bind_constant, false}, nullptr,
                                   &gpu->constants);
    }
    return status;
}

} // namespace

GpuStatus video_frame_gpu_present(VideoFrameGpu* gpu, GpuDevice* device,
    GpuContext1* context, const GpuVideoFrame& frame,
    GpuUav* destination, u32 x, u32 y, u32 step) noexcept {
    // The plane views are the decoder's one-slice array views of frame.subresource.
    if (!gpu || !device || !context || !frame.texture || !frame.luma || !frame.chroma || !destination ||
        frame.rotation > 3 ||
        frame.width == 0 || frame.height == 0 || step == 0 || step > kGpuVideoReductionMaximum) return E_INVALIDARG;
    // Reduced source extent; the written (rotated) extent swaps it for odd turns.
    const u32 width = gpu_video_frame_reduced(frame.width, step);
    const u32 height = gpu_video_frame_reduced(frame.height, step);
    const u32 output_width = gpu_video_frame_output_width(frame, step);
    const u32 output_height = gpu_video_frame_output_height(frame, step);
    D3D11_TEXTURE2D_DESC input{};
    frame.texture->GetDesc(&input);
    if (input.Format != DXGI_FORMAT_NV12 || input.MipLevels != 1 ||
        frame.subresource >= input.ArraySize ||
        !gpu_video_frame_geometry_valid(frame, input.Width, input.Height)) return E_INVALIDARG;
    ID3D11Device* source_device = nullptr;
    frame.texture->GetDevice(&source_device);
    const bool same_device = source_device == device;
    com_release(source_device);
    if (!same_device) return E_INVALIDARG;
    D3D11_UNORDERED_ACCESS_VIEW_DESC output_view{};
    destination->GetDesc(&output_view);
    if (output_view.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D ||
        output_view.Texture2D.MipSlice != 0 ||
        output_view.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return E_INVALIDARG;
    ID3D11Resource* resource = nullptr;
    destination->GetResource(&resource);
    ID3D11Texture2D* output = nullptr;
    GpuStatus status = resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&output));
    com_release(resource);
    if (FAILED(status)) return status;
    D3D11_TEXTURE2D_DESC target{};
    output->GetDesc(&target);
    com_release(output);
    if (target.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        x > target.Width || output_width > target.Width - x ||
        y > target.Height || output_height > target.Height - y) return E_INVALIDARG;
    status = ensure_resources(gpu, device);
    if (FAILED(status)) return status;
    auto* multithread = static_cast<ID3D11Multithread*>(gpu->context_lock);
    if (!multithread) {
        status = context->QueryInterface(__uuidof(ID3D11Multithread),
            reinterpret_cast<void**>(&multithread));
        if (FAILED(status)) return status;
        gpu->context_lock = multithread;
    }
    // Keep the short upload/conversion command sequence together instead of
    // repeatedly yielding the protected immediate context to decoder workers.
    // No decode, fence wait or resource creation occurs under this lock.
    multithread->Enter();
    void* mapped = gpu_map(context, gpu->constants, GpuMap::discard, &status);
    if (!mapped) { multithread->Leave(); return FAILED(status) ? status : kGpuFail; }
    const u32 lanes = video_frame_gpu_lanes(step);
    const VideoConstants constants{frame.crop_x, frame.crop_y, width, height, x, y,
        yuv_rgb_coefficients(frame.bt709, frame.full_range), step, frame.width, frame.height, frame.rotation,
        lanes, {}};
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
    multithread->Leave();
    ++gpu->frames;
    gpu->converted_pixels += static_cast<u64>(width) * height;
    return S_OK;
}

void video_frame_gpu_release(VideoFrameGpu* gpu) noexcept {
    if (!gpu) return;
    gpu_release(gpu->shader); gpu_release(gpu->constants);
    auto* multithread = static_cast<ID3D11Multithread*>(gpu->context_lock);
    com_release(multithread);
    reset_value<VideoFrameGpu>(gpu);
}

} // namespace maicord
