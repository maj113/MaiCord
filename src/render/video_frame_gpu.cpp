#include "maicord/render/video_frame_gpu.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/media/yuv_convert_common.hpp"
#include "maicord/render/shader_bytecode.hpp"
#include <d3d11_4.h>

namespace maicord {
namespace {

struct VideoConstants {
    u32 crop_x, crop_y, width, height, x, y;
    YuvRgbCoefficients color;
    u32 step, visible_width, visible_height, rotation;
};
static_assert(sizeof(VideoConstants) == 64);

HRESULT ensure_resources(VideoFrameGpu* gpu, ID3D11Device* device) noexcept {
    HRESULT status = S_OK;
    if (!gpu->shader) {
        status = shader_create_compute(device, ShaderId::video_frame, &gpu->shader);
    }
    if (SUCCEEDED(status) && !gpu->constants) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(VideoConstants);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        status = device->CreateBuffer(&desc, nullptr, &gpu->constants);
    }
    return status;
}

HRESULT ensure_nv12(VideoFrameGpu* gpu, ID3D11Device* device, u32 width, u32 height) noexcept {
    if (gpu->nv12 && gpu->width >= width && gpu->height >= height) return S_OK;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = maximum(width, gpu->width);
    desc.Height = maximum(height, gpu->height);
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* luma = nullptr;
    ID3D11ShaderResourceView* chroma = nullptr;
    HRESULT status = device->CreateTexture2D(&desc, nullptr, &texture);
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8_UNORM;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    if (SUCCEEDED(status)) status = device->CreateShaderResourceView(texture, &view, &luma);
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    if (SUCCEEDED(status)) status = device->CreateShaderResourceView(texture, &view, &chroma);
    if (FAILED(status)) {
        com_release(chroma); com_release(luma); com_release(texture);
        return status;
    }
    com_release(gpu->chroma); com_release(gpu->luma); com_release(gpu->nv12);
    gpu->nv12 = texture; gpu->luma = luma; gpu->chroma = chroma;
    gpu->width = desc.Width; gpu->height = desc.Height;
    ++gpu->allocations;
    return S_OK;
}

bool plane_matches(ID3D11ShaderResourceView* view, ID3D11Texture2D* texture, DXGI_FORMAT format) noexcept {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    view->GetDesc(&desc);
    if (desc.Format != format || desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        desc.Texture2D.MostDetailedMip != 0 || desc.Texture2D.MipLevels != 1) return false;
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    const bool matches = resource == static_cast<ID3D11Resource*>(texture);
    com_release(resource);
    return matches;
}

bool direct_planes(const GpuVideoFrame& frame, const D3D11_TEXTURE2D_DESC& input) noexcept {
    return frame.luma && frame.chroma && input.ArraySize == 1 && frame.subresource == 0 &&
           (input.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
           plane_matches(frame.luma, frame.texture, DXGI_FORMAT_R8_UNORM) &&
           plane_matches(frame.chroma, frame.texture, DXGI_FORMAT_R8G8_UNORM);
}

} // namespace

HRESULT video_frame_gpu_present(VideoFrameGpu* gpu, ID3D11Device* device,
    ID3D11DeviceContext1* context, const GpuVideoFrame& frame,
    ID3D11UnorderedAccessView* destination, u32 x, u32 y, u32 step) noexcept {
    if (!gpu || !device || !context || !frame.texture || !destination || frame.rotation > 3 ||
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
    HRESULT status = resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&output));
    com_release(resource);
    if (FAILED(status)) return status;
    D3D11_TEXTURE2D_DESC target{};
    output->GetDesc(&target);
    com_release(output);
    if (target.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        x > target.Width || output_width > target.Width - x ||
        y > target.Height || output_height > target.Height - y) return E_INVALIDARG;
    const bool direct = direct_planes(frame, input);
    status = ensure_resources(gpu, device);
    if (SUCCEEDED(status) && !direct) status = ensure_nv12(gpu, device, input.Width, input.Height);
    if (FAILED(status)) return status;
    if (!gpu->multithread) {
        status = context->QueryInterface(__uuidof(ID3D11Multithread),
            reinterpret_cast<void**>(&gpu->multithread));
        if (FAILED(status)) return status;
    }
    // Keep the short upload/conversion command sequence together instead of
    // repeatedly yielding the protected immediate context to decoder workers.
    // No decode, fence wait or resource creation occurs under this lock.
    gpu->multithread->Enter();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    status = context->Map(gpu->constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(status)) { gpu->multithread->Leave(); return status; }
    const VideoConstants constants{frame.crop_x, frame.crop_y, width, height, x, y,
        yuv_rgb_coefficients(frame.bt709, frame.full_range), step, frame.width, frame.height, frame.rotation};
    copy_bytes(mapped.pData, &constants, sizeof(constants));
    context->Unmap(gpu->constants, 0);
    if (!direct) {
        const D3D11_BOX box{0, 0, 0, input.Width, input.Height, 1};
        context->CopySubresourceRegion(gpu->nv12, 0, 0, 0, 0,
                                      frame.texture, frame.subresource, &box);
    }
    ID3D11ShaderResourceView* inputs[]{direct ? frame.luma : gpu->luma, direct ? frame.chroma : gpu->chroma};
    // The atlas can still be bound by the preceding draw. Unbind it before
    // using the UAV; leave all compute bindings clean for the next render pass.
    ID3D11ShaderResourceView* empty[3]{};
    context->PSSetShaderResources(0, 3, empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CSSetShader(gpu->shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &gpu->constants);
    context->CSSetShaderResources(0, 2, inputs);
    context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
    context->Dispatch((output_width + 7u) / 8u, (output_height + 7u) / 8u, 1);
    context->CSSetShaderResources(0, 2, empty);
    ID3D11UnorderedAccessView* no_uav = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
    ID3D11Buffer* no_buffer = nullptr;
    context->CSSetConstantBuffers(0, 1, &no_buffer);
    context->CSSetShader(nullptr, nullptr, 0);
    gpu->multithread->Leave();
    ++gpu->frames;
    if (!direct) gpu->copied_bytes += static_cast<u64>(input.Width) * input.Height * 3u / 2u;
    gpu->converted_pixels += static_cast<u64>(width) * height;
    return S_OK;
}

void video_frame_gpu_release(VideoFrameGpu* gpu) noexcept {
    if (!gpu) return;
    com_release(gpu->chroma); com_release(gpu->luma); com_release(gpu->nv12);
    com_release(gpu->shader); com_release(gpu->constants);
    com_release(gpu->multithread);
    *gpu = VideoFrameGpu{};
}

} // namespace maicord
