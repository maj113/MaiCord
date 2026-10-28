#include "gpu_decoder_surfaces.hpp"
#include "maicord/media/h264_output_queue.hpp"
#include "maicord/media/video_display.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/sys/initguid.hpp"
#include "maicord/sys/dxva.hpp"

namespace maicord {

bool gpu_decoder_surfaces_supported(ID3D11Device* device) noexcept {
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD |
                              D3D11_FORMAT_SUPPORT_DECODER_OUTPUT;
    UINT support = 0;
    return device && SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) &&
           (support & required) == required;
}

HRESULT gpu_decoder_select_config(ID3D11VideoDevice* video, const D3D11_VIDEO_DECODER_DESC& description,
                                  u8 bitstream_raw, D3D11_VIDEO_DECODER_CONFIG* selected) noexcept {
    UINT count = 0;
    HRESULT status = video->GetVideoDecoderConfigCount(&description, &count);
    for (UINT i = 0; SUCCEEDED(status) && i < count; ++i) {
        D3D11_VIDEO_DECODER_CONFIG candidate{};
        status = video->GetVideoDecoderConfig(&description, i, &candidate);
        if (SUCCEEDED(status) && candidate.ConfigBitstreamRaw == bitstream_raw &&
            candidate.guidConfigBitstreamEncryption == DXVA_NoEncrypt) {
            *selected = candidate;
            return S_OK;
        }
    }
    return FAILED(status) ? status : DXGI_ERROR_UNSUPPORTED;
}

HRESULT gpu_decoder_upload_buffer(ID3D11VideoContext* context, ID3D11VideoDecoder* decoder,
                                  D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void* data, u32 bytes,
                                  D3D11_VIDEO_DECODER_BUFFER_DESC* description) noexcept {
    UINT capacity = 0;
    void* destination = nullptr;
    HRESULT status = context->GetDecoderBuffer(decoder, type, &capacity, &destination);
    if (FAILED(status))
        return status;
    const bool fits = destination && bytes <= capacity;
    if (fits)
        copy_bytes(destination, data, bytes);
    status = context->ReleaseDecoderBuffer(decoder, type);
    if (!fits)
        return E_OUTOFMEMORY;
    reset_value(description);
    description->BufferType = type;
    description->DataSize = bytes;
    return status;
}

Mp4DecodedFrame gpu_decoder_output_frame(H264OutputQueue* output, GpuVideoFrame* frame,
                                         const GpuDecoderSurfaces& surfaces,
                                         const VideoDisplayExtent& display) noexcept {
    H264Output picture{};
    Mp4DecodedFrame decoded{};
    if (!h264_output_pop(output, &picture)) {
        decoded.error = Mp4DecodeError::sample;
        return decoded;
    }
    frame->subresource = picture.surface;
    frame->luma = surfaces.luma[picture.surface];
    frame->chroma = surfaces.chroma[picture.surface];
    decoded.gpu = frame;
    decoded.width = frame->width;
    decoded.height = frame->height;
    decoded.display_width = display.width;
    decoded.display_height = display.height;
    decoded.duration_us = decoded.source_duration_us = picture.duration_us;
    decoded.presentation_time_us = picture.presentation_time_us;
    return decoded;
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
