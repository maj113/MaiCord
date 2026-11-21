#include "../video_frame_convert.hpp"

#include "maicord/sys/d3d11.hpp"

// The Direct3D 11 conversion of a decoder's NV12 frame: the frame's texture, plane views and the destination's
// format are validated against the D3D11 objects themselves, and the immediate context is locked through its
// ID3D11Multithread (decode workers share it). The OpenGL ES build converts imported planes in its own backend.

namespace maicord {

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
    // The Get* calls fill their whole output (a device child always has a device and a view its resource).
    D3D11_TEXTURE2D_DESC input;
    frame.texture->GetDesc(&input);
    if (input.Format != DXGI_FORMAT_NV12 || input.MipLevels != 1 ||
        frame.subresource >= input.ArraySize ||
        !gpu_video_frame_geometry_valid(frame, input.Width, input.Height)) return E_INVALIDARG;
    ID3D11Device* source_device;
    frame.texture->GetDevice(&source_device);
    const bool same_device = source_device == device;
    source_device->Release();
    if (!same_device) return E_INVALIDARG;
    D3D11_UNORDERED_ACCESS_VIEW_DESC output_view;
    destination->GetDesc(&output_view);
    if (output_view.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D ||
        output_view.Texture2D.MipSlice != 0 ||
        output_view.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return E_INVALIDARG;
    ID3D11Resource* resource;
    destination->GetResource(&resource);
    ID3D11Texture2D* output;
    GpuStatus status = resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&output));
    resource->Release();
    if (FAILED(status)) return status;
    D3D11_TEXTURE2D_DESC target;
    output->GetDesc(&target);
    output->Release();
    if (target.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        x > target.Width || output_width > target.Width - x ||
        y > target.Height || output_height > target.Height - y) return E_INVALIDARG;
    status = video_frame_resources(gpu, device);
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
    status = video_frame_convert(gpu, context, frame, destination, x, y, step, width, height, output_width,
                                 output_height, status);
    multithread->Leave();
    return status;
}

void video_frame_gpu_release(VideoFrameGpu* gpu) noexcept {
    if (!gpu) return;
    gpu_release(gpu->shader); gpu_release(gpu->constants);
    auto* multithread = static_cast<ID3D11Multithread*>(gpu->context_lock);
    com_release(multithread);
    reset_value<VideoFrameGpu>(gpu);
}

} // namespace maicord
