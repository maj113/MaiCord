#pragma once

// Shader-readable NV12 test picture with the one-slice plane views the video
// converter requires (decoders hand the renderer the same kind of frame).
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {

struct Nv12TestPicture {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* luma = nullptr;
    ID3D11ShaderResourceView* chroma = nullptr;

    // pixels: tightly packed NV12 (luma rows, then interleaved chroma rows).
    bool create(ID3D11Device* device, u32 width, u32 height, const void* pixels) noexcept {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{pixels, width, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8_UNORM;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.ArraySize = 1;
        bool ok = SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)) &&
                  SUCCEEDED(device->CreateShaderResourceView(texture, &view, &luma));
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        ok = ok && SUCCEEDED(device->CreateShaderResourceView(texture, &view, &chroma));
        if (!ok)
            release();
        return ok;
    }
    GpuVideoFrame frame(u32 width, u32 height) const noexcept {
        GpuVideoFrame value{};
        value.texture = texture;
        value.width = width;
        value.height = height;
        value.luma = luma;
        value.chroma = chroma;
        return value;
    }
    void release() noexcept {
        com_release(chroma);
        com_release(luma);
        com_release(texture);
    }
};

} // namespace maicord
