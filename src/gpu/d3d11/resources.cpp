#include "maicord/gpu/api.hpp"

// The Direct3D 11 creation calls that fill a description: once per resource, so one body for every caller.
namespace maicord {

GpuStatus gpu_buffer_create(GpuDevice* device, const GpuBufferDesc& desc, const void* initial, GpuBuffer** buffer) {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = desc.bytes;
    description.Usage = gpu_d3d11::usage(desc.usage);
    description.BindFlags = desc.bind;
    description.CPUAccessFlags = desc.usage == GpuUsage::dynamic        ? D3D11_CPU_ACCESS_WRITE
                                 : desc.usage == GpuUsage::staging_read ? D3D11_CPU_ACCESS_READ
                                                                        : 0;
    description.MiscFlags = desc.raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : 0;
    const D3D11_SUBRESOURCE_DATA data{initial, 0, 0};
    return device->CreateBuffer(&description, initial ? &data : nullptr, buffer);
}

GpuStatus gpu_texture_create(GpuDevice* device, const GpuTextureDesc& desc, const GpuInitialData* initial,
                             GpuTexture** texture) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = desc.width;
    description.Height = desc.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = gpu_d3d11::dxgi_format(desc.format);
    description.SampleDesc.Count = 1;
    description.Usage = gpu_d3d11::usage(desc.usage);
    description.BindFlags = desc.bind;
    description.CPUAccessFlags = desc.usage == GpuUsage::staging_read ? D3D11_CPU_ACCESS_READ : 0;
    const D3D11_SUBRESOURCE_DATA data{initial ? initial->data : nullptr, initial ? initial->pitch : 0, 0};
    return device->CreateTexture2D(&description, initial ? &data : nullptr, texture);
}

GpuStatus gpu_srv_create(GpuDevice* device, GpuBuffer* buffer, const GpuViewDesc& desc, GpuSrv** view) {
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    if (desc.kind == GpuViewKind::buffer_raw) {
        description.Format = DXGI_FORMAT_R32_TYPELESS;
        description.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        description.BufferEx.NumElements = desc.elements;
        description.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    } else {
        description.Format = gpu_d3d11::dxgi_format(desc.format);
        description.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        description.Buffer.NumElements = desc.elements;
    }
    return device->CreateShaderResourceView(buffer, &description, view);
}

GpuStatus gpu_uav_create(GpuDevice* device, GpuBuffer* buffer, u32 elements, GpuUav** view) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    description.Buffer.NumElements = elements;
    description.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return device->CreateUnorderedAccessView(buffer, &description, view);
}

GpuStatus gpu_sampler_create(GpuDevice* device, const GpuSamplerDesc& desc, GpuSampler** sampler) {
    D3D11_SAMPLER_DESC description{};
    description.Filter = static_cast<D3D11_FILTER>(desc.filter);
    description.AddressU = description.AddressV = description.AddressW =
        static_cast<D3D11_TEXTURE_ADDRESS_MODE>(desc.address);
    description.MaxLOD = 0.0f;
    return device->CreateSamplerState(&description, sampler);
}

GpuStatus gpu_blend_create(GpuDevice* device, const GpuBlendDesc& desc, GpuBlend** blend) {
    D3D11_BLEND_DESC description{};
    D3D11_RENDER_TARGET_BLEND_DESC& target = description.RenderTarget[0];
    target.BlendEnable = desc.enable ? TRUE : FALSE;
    target.SrcBlend = gpu_d3d11::blend_factor(desc.source);
    target.DestBlend = gpu_d3d11::blend_factor(desc.destination);
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = gpu_d3d11::blend_factor(desc.source_alpha);
    target.DestBlendAlpha = gpu_d3d11::blend_factor(desc.destination_alpha);
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return device->CreateBlendState(&description, blend);
}

GpuStatus gpu_raster_create(GpuDevice* device, GpuRaster** raster) {
    D3D11_RASTERIZER_DESC description{};
    description.FillMode = D3D11_FILL_SOLID;
    description.CullMode = D3D11_CULL_NONE;
    description.DepthClipEnable = TRUE;
    description.ScissorEnable = TRUE;
    return device->CreateRasterizerState(&description, raster);
}

} // namespace maicord
