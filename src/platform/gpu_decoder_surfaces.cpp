#include "gpu_decoder_surfaces.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {

bool gpu_decoder_surfaces_supported(ID3D11Device* device) noexcept {
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD |
                              D3D11_FORMAT_SUPPORT_DECODER_OUTPUT;
    UINT support = 0;
    return device && SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) &&
           (support & required) == required;
}

HRESULT gpu_decoder_surfaces_create(GpuDecoderSurfaces* surfaces, ID3D11Device* device,
                                    ID3D11VideoDevice* video, const GUID& profile, u32 width, u32 height,
                                    u32 count, u32 output_views) noexcept {
    if (!surfaces || !device || !video || surfaces->texture || !count || output_views > count)
        return E_INVALIDARG;
    if (count > kGpuDecoderSurfaceCapacity)
        return DXGI_ERROR_UNSUPPORTED;
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = width;
    texture.Height = height;
    texture.ArraySize = count;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_NV12;
    texture.SampleDesc.Count = 1;
    texture.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    HRESULT status = device->CreateTexture2D(&texture, nullptr, &surfaces->texture);
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC output{};
    output.DecodeProfile = profile;
    output.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
    for (u32 i = 0; i < output_views && SUCCEEDED(status); ++i) {
        output.Texture2D.ArraySlice = i;
        status = video->CreateVideoDecoderOutputView(surfaces->texture, &output, &surfaces->views[i]);
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC plane{};
    plane.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    plane.Texture2DArray.MipLevels = 1;
    plane.Texture2DArray.ArraySize = 1;
    for (u32 i = 0; i < count && SUCCEEDED(status); ++i) {
        plane.Texture2DArray.FirstArraySlice = i;
        plane.Format = DXGI_FORMAT_R8_UNORM;
        status = device->CreateShaderResourceView(surfaces->texture, &plane, &surfaces->luma[i]);
        plane.Format = DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(status))
            status = device->CreateShaderResourceView(surfaces->texture, &plane, &surfaces->chroma[i]);
    }
    if (FAILED(status)) {
        gpu_decoder_surfaces_release(surfaces);
        // A driver that refuses shader-readable decoder output has no usable decoder.
        return status == E_OUTOFMEMORY ? status : DXGI_ERROR_UNSUPPORTED;
    }
    surfaces->count = static_cast<u8>(count);
    return S_OK;
}

void gpu_decoder_surfaces_release(GpuDecoderSurfaces* surfaces) noexcept {
    if (!surfaces)
        return;
    for (u32 i = 0; i < kGpuDecoderSurfaceCapacity; ++i) {
        com_release(surfaces->chroma[i]);
        com_release(surfaces->luma[i]);
        com_release(surfaces->views[i]);
    }
    com_release(surfaces->texture);
    surfaces->count = 0;
}

} // namespace maicord
