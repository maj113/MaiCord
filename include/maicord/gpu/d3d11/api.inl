// The Direct3D 11 bodies of the MAICORD_GPU_INLINE functions of maicord/gpu/api.hpp and surface.hpp: each is the
// D3D11 call it stands for (a create call fills the D3D11 description from the portable one, which folds away where
// the description is a constant), so a call site compiles to what the code it replaced did. Included at the end of
// surface.hpp; the out-of-line functions (device, shaders, surface lifetime) are in src/gpu/d3d11.

#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {

static_assert(sizeof(GpuRect) == sizeof(D3D11_RECT) && __builtin_offsetof(GpuRect, left) == __builtin_offsetof(RECT, left) &&
              __builtin_offsetof(GpuRect, top) == __builtin_offsetof(RECT, top) &&
              __builtin_offsetof(GpuRect, right) == __builtin_offsetof(RECT, right) &&
              __builtin_offsetof(GpuRect, bottom) == __builtin_offsetof(RECT, bottom));

namespace gpu_d3d11 {

// api.hpp gives the enumerators Direct3D 11's values, so a cast converts (and costs nothing); these keep it true.
static_assert(static_cast<UINT>(GpuFormat::rgba8) == DXGI_FORMAT_R8G8B8A8_UNORM &&
              static_cast<UINT>(GpuFormat::bgra8) == DXGI_FORMAT_B8G8R8A8_UNORM &&
              static_cast<UINT>(GpuFormat::r8) == DXGI_FORMAT_R8_UNORM &&
              static_cast<UINT>(GpuFormat::rg8) == DXGI_FORMAT_R8G8_UNORM &&
              static_cast<UINT>(GpuFormat::rgba16f) == DXGI_FORMAT_R16G16B16A16_FLOAT &&
              static_cast<UINT>(GpuFormat::rgba32f) == DXGI_FORMAT_R32G32B32A32_FLOAT &&
              static_cast<UINT>(GpuFormat::r32_uint) == DXGI_FORMAT_R32_UINT);
static_assert(static_cast<UINT>(GpuUsage::default_) == D3D11_USAGE_DEFAULT &&
              static_cast<UINT>(GpuUsage::immutable) == D3D11_USAGE_IMMUTABLE &&
              static_cast<UINT>(GpuUsage::dynamic) == D3D11_USAGE_DYNAMIC &&
              static_cast<UINT>(GpuUsage::staging_read) == D3D11_USAGE_STAGING);
static_assert(static_cast<UINT>(gpu_bind_vertex) == D3D11_BIND_VERTEX_BUFFER &&
              static_cast<UINT>(gpu_bind_constant) == D3D11_BIND_CONSTANT_BUFFER &&
              static_cast<UINT>(gpu_bind_srv) == D3D11_BIND_SHADER_RESOURCE &&
              static_cast<UINT>(gpu_bind_rtv) == D3D11_BIND_RENDER_TARGET &&
              static_cast<UINT>(gpu_bind_uav) == D3D11_BIND_UNORDERED_ACCESS);
static_assert(static_cast<UINT>(GpuBlendFactor::zero) == D3D11_BLEND_ZERO &&
              static_cast<UINT>(GpuBlendFactor::one) == D3D11_BLEND_ONE &&
              static_cast<UINT>(GpuBlendFactor::src_color) == D3D11_BLEND_SRC_COLOR &&
              static_cast<UINT>(GpuBlendFactor::inv_src_color) == D3D11_BLEND_INV_SRC_COLOR &&
              static_cast<UINT>(GpuBlendFactor::src_alpha) == D3D11_BLEND_SRC_ALPHA &&
              static_cast<UINT>(GpuBlendFactor::inv_src_alpha) == D3D11_BLEND_INV_SRC_ALPHA &&
              static_cast<UINT>(GpuBlendFactor::blend_factor) == D3D11_BLEND_BLEND_FACTOR &&
              static_cast<UINT>(GpuBlendFactor::src1_color) == D3D11_BLEND_SRC1_COLOR);
static_assert(static_cast<UINT>(GpuFilter::point) == D3D11_FILTER_MIN_MAG_MIP_POINT &&
              static_cast<UINT>(GpuFilter::linear) == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT &&
              static_cast<UINT>(GpuAddress::wrap) == D3D11_TEXTURE_ADDRESS_WRAP &&
              static_cast<UINT>(GpuAddress::clamp) == D3D11_TEXTURE_ADDRESS_CLAMP);

__forceinline constexpr DXGI_FORMAT dxgi_format(GpuFormat format) noexcept {
    return static_cast<DXGI_FORMAT>(format);
}

// The portable format of a texture's DXGI format (every texture the renderer makes has one of them).
__forceinline constexpr GpuFormat gpu_format(DXGI_FORMAT format) noexcept {
    return static_cast<GpuFormat>(format);
}

__forceinline constexpr D3D11_USAGE usage(GpuUsage value) noexcept {
    return static_cast<D3D11_USAGE>(value);
}

__forceinline constexpr UINT bind_flags(u8 bind) noexcept {
    return bind;
}

__forceinline constexpr D3D11_BLEND blend_factor(GpuBlendFactor factor) noexcept {
    return static_cast<D3D11_BLEND>(factor);
}

} // namespace gpu_d3d11

// ---- Device ----------------------------------------------------------------------------------------------

__forceinline GpuDevice* gpu_context_device(GpuContext* context) {
    GpuDevice* device = nullptr;
    context->GetDevice(&device);
    if (device)
        device->Release();
    return device;
}

__forceinline GpuStatus gpu_format_check(GpuDevice* device, GpuFormat format, u8 bind) {
    UINT support = 0;
    const HRESULT result = device->CheckFormatSupport(gpu_d3d11::dxgi_format(format), &support);
    UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D;
    if (bind & gpu_bind_srv)
        required |= D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (bind & gpu_bind_rtv)
        required |= D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    if (bind & gpu_bind_uav)
        required |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if (FAILED(result) || (support & required) != required)
        return FAILED(result) ? result : kGpuUnsupported;
    return S_OK;
}

// ---- Resources -------------------------------------------------------------------------------------------

__forceinline GpuStatus gpu_buffer_create(GpuDevice* device, const GpuBufferDesc& desc, const void* initial,
                                          GpuBuffer** buffer) {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = desc.bytes;
    description.Usage = gpu_d3d11::usage(desc.usage);
    description.BindFlags = gpu_d3d11::bind_flags(desc.bind);
    description.CPUAccessFlags = desc.usage == GpuUsage::dynamic        ? D3D11_CPU_ACCESS_WRITE
                                 : desc.usage == GpuUsage::staging_read ? D3D11_CPU_ACCESS_READ
                                                                        : 0;
    description.MiscFlags = desc.raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : 0;
    const D3D11_SUBRESOURCE_DATA data{initial, 0, 0};
    return device->CreateBuffer(&description, initial ? &data : nullptr, buffer);
}

__forceinline GpuStatus gpu_texture_create(GpuDevice* device, const GpuTextureDesc& desc, const GpuInitialData* initial,
                                           GpuTexture** texture) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = desc.width;
    description.Height = desc.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = gpu_d3d11::dxgi_format(desc.format);
    description.SampleDesc.Count = 1;
    description.Usage = gpu_d3d11::usage(desc.usage);
    description.BindFlags = gpu_d3d11::bind_flags(desc.bind);
    description.CPUAccessFlags = desc.usage == GpuUsage::staging_read ? D3D11_CPU_ACCESS_READ : 0;
    const D3D11_SUBRESOURCE_DATA data{initial ? initial->data : nullptr, initial ? initial->pitch : 0, 0};
    return device->CreateTexture2D(&description, initial ? &data : nullptr, texture);
}

struct GpuTextureInfo {
    D3D11_TEXTURE2D_DESC description;
};

__forceinline void gpu_texture_info(GpuTexture* texture, GpuTextureInfo* info) {
    texture->GetDesc(&info->description);
}

__forceinline u32 gpu_texture_info_width(const GpuTextureInfo& info) {
    return info.description.Width;
}

__forceinline u32 gpu_texture_info_height(const GpuTextureInfo& info) {
    return info.description.Height;
}

__forceinline GpuStatus gpu_texture_create_from_info(GpuDevice* device, GpuTextureInfo& info, u32 height,
                                                     bool copy_only, GpuTexture** texture) {
    info.description.Height = height;
    if (copy_only) {
        info.description.BindFlags = 0;
        info.description.CPUAccessFlags = 0;
        info.description.MiscFlags = 0;
    }
    return device->CreateTexture2D(&info.description, nullptr, texture);
}

// A texture view covers the whole texture in the texture's own format.
__forceinline GpuStatus gpu_srv_create(GpuDevice* device, GpuTexture* texture, const GpuViewDesc&, GpuSrv** view) {
    return device->CreateShaderResourceView(texture, nullptr, view);
}

__forceinline GpuStatus gpu_srv_create(GpuDevice* device, GpuBuffer* buffer, const GpuViewDesc& desc, GpuSrv** view) {
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

__forceinline GpuStatus gpu_uav_create(GpuDevice* device, GpuTexture* texture, GpuUav** view) {
    return device->CreateUnorderedAccessView(texture, nullptr, view);
}

__forceinline GpuStatus gpu_uav_create(GpuDevice* device, GpuBuffer* buffer, u32 elements, GpuUav** view) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    description.Buffer.NumElements = elements;
    description.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return device->CreateUnorderedAccessView(buffer, &description, view);
}

__forceinline GpuStatus gpu_rtv_create(GpuDevice* device, GpuTexture* texture, GpuRtv** view) {
    return device->CreateRenderTargetView(texture, nullptr, view);
}

__forceinline GpuStatus gpu_sampler_create(GpuDevice* device, const GpuSamplerDesc& desc, GpuSampler** sampler) {
    D3D11_SAMPLER_DESC description{};
    description.Filter = static_cast<D3D11_FILTER>(desc.filter);
    description.AddressU = description.AddressV = description.AddressW =
        static_cast<D3D11_TEXTURE_ADDRESS_MODE>(desc.address);
    description.MaxLOD = 0.0f;
    return device->CreateSamplerState(&description, sampler);
}

__forceinline GpuStatus gpu_blend_create(GpuDevice* device, const GpuBlendDesc& desc, GpuBlend** blend) {
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

__forceinline GpuStatus gpu_raster_create(GpuDevice* device, GpuRaster** raster) {
    D3D11_RASTERIZER_DESC description{};
    description.FillMode = D3D11_FILL_SOLID;
    description.CullMode = D3D11_CULL_NONE;
    description.DepthClipEnable = TRUE;
    description.ScissorEnable = TRUE;
    return device->CreateRasterizerState(&description, raster);
}

// ---- Commands --------------------------------------------------------------------------------------------

// One view with a null entry unbinds the slot, as binding no target does.
__forceinline void gpu_set_render_target(GpuContext* context, GpuRtv* target) {
    context->OMSetRenderTargets(1, &target, nullptr);
}

__forceinline void gpu_set_viewport(GpuContext* context, float x, float y, float width, float height) {
    const D3D11_VIEWPORT viewport{x, y, width, height, 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
}

__forceinline void gpu_set_scissor(GpuContext* context, const GpuRect& rectangle) {
    context->RSSetScissorRects(1, reinterpret_cast<const D3D11_RECT*>(&rectangle));
}

__forceinline void gpu_set_raster(GpuContext* context, GpuRaster* raster) {
    context->RSSetState(raster);
}

__forceinline void gpu_set_blend(GpuContext* context, GpuBlend* blend, const float factor[4]) {
    context->OMSetBlendState(blend, factor, 0xFFFFFFFFu);
}

__forceinline void gpu_set_input_layout(GpuContext* context, GpuInputLayout* layout) {
    context->IASetInputLayout(layout);
}

__forceinline void gpu_set_vertex_buffer(GpuContext* context, GpuBuffer* buffer, u32 stride, u32 offset) {
    context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
}

__forceinline void gpu_set_strip_topology(GpuContext* context) {
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
}

__forceinline void gpu_set_shader(GpuContext* context, GpuVertexShader* shader) {
    context->VSSetShader(shader, nullptr, 0);
}

__forceinline void gpu_set_shader(GpuContext* context, GpuPixelShader* shader) {
    context->PSSetShader(shader, nullptr, 0);
}

__forceinline void gpu_set_shader(GpuContext* context, GpuComputeShader* shader) {
    context->CSSetShader(shader, nullptr, 0);
}

__forceinline void gpu_set_constants(GpuContext* context, GpuStage stage, u32 first, u32 count,
                                     GpuBuffer* const* buffers) {
    switch (stage) {
    case GpuStage::vertex:
        context->VSSetConstantBuffers(first, count, buffers);
        break;
    case GpuStage::pixel:
        context->PSSetConstantBuffers(first, count, buffers);
        break;
    case GpuStage::compute:
        context->CSSetConstantBuffers(first, count, buffers);
        break;
    }
}

__forceinline void gpu_set_constants_range(GpuContext1* context, u32 slot, GpuBuffer* buffer, u32 first_constant,
                                           u32 constant_count) {
    context->CSSetConstantBuffers1(slot, 1, &buffer, &first_constant, &constant_count);
}

__forceinline void gpu_set_srvs(GpuContext* context, GpuStage stage, u32 first, u32 count, GpuSrv* const* views) {
    switch (stage) {
    case GpuStage::vertex:
        context->VSSetShaderResources(first, count, views);
        break;
    case GpuStage::pixel:
        context->PSSetShaderResources(first, count, views);
        break;
    case GpuStage::compute:
        context->CSSetShaderResources(first, count, views);
        break;
    }
}

// Compute shaders in this program sample nothing through a sampler, so only the graphics stages take them.
__forceinline void gpu_set_samplers(GpuContext* context, GpuStage stage, u32 first, u32 count,
                                    GpuSampler* const* samplers) {
    if (stage == GpuStage::vertex)
        context->VSSetSamplers(first, count, samplers);
    else if (stage == GpuStage::pixel)
        context->PSSetSamplers(first, count, samplers);
}

__forceinline void gpu_set_uavs(GpuContext* context, u32 first, u32 count, GpuUav* const* views) {
    context->CSSetUnorderedAccessViews(first, count, views, nullptr);
}

__forceinline void gpu_draw(GpuContext* context, u32 vertices, u32 first_vertex) {
    context->Draw(vertices, first_vertex);
}

__forceinline void gpu_draw_instanced(GpuContext* context, u32 vertices, u32 instances, u32 first_vertex,
                                      u32 first_instance) {
    context->DrawInstanced(vertices, instances, first_vertex, first_instance);
}

__forceinline void gpu_dispatch(GpuContext* context, u32 x, u32 y, u32 z) {
    context->Dispatch(x, y, z);
}

__forceinline void gpu_clear_rtv(GpuContext* context, GpuRtv* target, const float rgba[4]) {
    context->ClearRenderTargetView(target, rgba);
}

__forceinline void gpu_clear_rtv_rect(GpuContext1* context, GpuRtv* target, const float rgba[4],
                                      const GpuRect& rectangle) {
    context->ClearView(target, rgba, reinterpret_cast<const D3D11_RECT*>(&rectangle), 1);
}

__forceinline void gpu_update_buffer(GpuContext* context, GpuBuffer* buffer, const void* data, u32) {
    context->UpdateSubresource(buffer, 0, nullptr, data, 0, 0);
}

__forceinline void* gpu_map(GpuContext* context, GpuBuffer* buffer, GpuMap mode, GpuStatus* status) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result =
        context->Map(buffer, 0, mode == GpuMap::discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0,
                     &mapped);
    if (status)
        *status = result;
    if (FAILED(result))
        return nullptr;
    return mapped.pData;
}

__forceinline void gpu_unmap(GpuContext* context, GpuBuffer* buffer) {
    context->Unmap(buffer, 0);
}

__forceinline void gpu_copy_region(GpuContext* context, GpuTexture* destination, u32 x, u32 y, GpuTexture* source,
                                   const GpuBox& box) {
    const D3D11_BOX region{box.left, box.top, 0, box.right, box.bottom, 1};
    context->CopySubresourceRegion(destination, 0, x, y, 0, source, 0, &region);
}

// Copies the box into a staging texture and maps it for reading. The caller names the source's format, so the staging
// texture is made without asking the source for its description.
__forceinline GpuStatus gpu_readback_map(GpuDevice* device, GpuContext* context, GpuTexture* source, GpuFormat format,
                                         const GpuBox& box, GpuReadback* readback) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = box.right - box.left;
    description.Height = box.bottom - box.top;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = gpu_d3d11::dxgi_format(format);
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    HRESULT result = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result))
        return result;
    const D3D11_BOX region{box.left, box.top, 0, box.right, box.bottom, 1};
    context->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &region);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        staging->Release();
        return result;
    }
    readback->data = mapped.pData;
    readback->pitch = mapped.RowPitch;
    readback->format = format;
    readback->backend = staging;
    return S_OK;
}

__forceinline void gpu_readback_unmap(GpuContext* context, GpuReadback* readback) {
    auto* staging = static_cast<ID3D11Texture2D*>(readback->backend);
    context->Unmap(staging, 0);
    staging->Release();
}

__forceinline GpuStatus gpu_readback(GpuDevice* device, GpuContext* context, GpuTexture* source, const GpuBox& box,
                                     void* destination, u32 pitch) {
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    GpuReadback readback{};
    const HRESULT result =
        gpu_readback_map(device, context, source, gpu_d3d11::gpu_format(description.Format), box, &readback);
    if (SUCCEEDED(result)) {
        const u32 height = box.bottom - box.top;
        const u32 row_bytes = (box.right - box.left) * gpu_texel_bytes(readback.format);
        for (u32 row = 0; row < height; ++row)
            copy_bytes(static_cast<u8*>(destination) + static_cast<usize>(row) * pitch,
                       static_cast<const u8*>(readback.data) + static_cast<usize>(row) * readback.pitch, row_bytes);
        gpu_readback_unmap(context, &readback);
    }
    return result;
}

__forceinline void gpu_flush(GpuContext* context) {
    context->Flush();
}

__forceinline void gpu_clear_state(GpuContext* context) {
    context->ClearState();
}

// ---- Frame timing and fences -------------------------------------------------------------------------------

__forceinline GpuStatus gpu_timer_create(GpuDevice* device, GpuTimer* timer) {
    const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    const D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
    HRESULT result = device->CreateQuery(&disjoint, &timer->disjoint);
    if (SUCCEEDED(result))
        result = device->CreateQuery(&stamp, &timer->begin);
    if (SUCCEEDED(result))
        result = device->CreateQuery(&stamp, &timer->end);
    return result;
}

__forceinline bool gpu_timer_valid(const GpuTimer& timer) {
    return timer.disjoint != nullptr;
}

__forceinline void gpu_timer_release(GpuTimer* timer) {
    com_release(timer->disjoint);
    com_release(timer->begin);
    com_release(timer->end);
}

__forceinline void gpu_timer_begin(GpuContext* context, GpuTimer* timer) {
    context->Begin(timer->disjoint);
    context->End(timer->begin);
}

__forceinline void gpu_timer_end(GpuContext* context, GpuTimer* timer) {
    context->End(timer->end);
    context->End(timer->disjoint);
}

__forceinline GpuPoll gpu_timer_poll(GpuContext* context, GpuTimer* timer, bool flush, double* milliseconds) {
    const UINT flags = flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    const HRESULT state = context->GetData(timer->disjoint, &disjoint, sizeof(disjoint), flags);
    u64 begin = 0, end = 0;
    HRESULT a = S_OK, b = S_OK;
    if (state != S_FALSE && SUCCEEDED(state)) {
        a = context->GetData(timer->begin, &begin, sizeof(begin), flags);
        b = context->GetData(timer->end, &end, sizeof(end), flags);
    }
    if (state == S_FALSE || a == S_FALSE || b == S_FALSE)
        return GpuPoll::pending;
    if (FAILED(state) || FAILED(a) || FAILED(b) || disjoint.Disjoint || !disjoint.Frequency || end < begin)
        return GpuPoll::invalid;
    *milliseconds = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
    return GpuPoll::ready;
}

// The immediate context's signalling interface and a fence of the same device, made in the order they have always
// been (the signaller is left set when the fence then fails, for the caller to release).
__forceinline GpuStatus gpu_fence_create(GpuDevice* device, GpuContext4** signaller, GpuFence** fence) {
    ID3D11DeviceContext* context = nullptr;
    ID3D11Device5* device5 = nullptr;
    device->GetImmediateContext(&context);
    HRESULT result = context ? context->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                                       reinterpret_cast<void**>(signaller))
                             : E_NOINTERFACE;
    if (SUCCEEDED(result))
        result = device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5));
    if (SUCCEEDED(result))
        result = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void**>(fence));
    if (device5)
        device5->Release();
    if (context)
        context->Release();
    return result;
}

__forceinline GpuStatus gpu_fence_signal(GpuContext4* signaller, GpuFence* fence, u64 value) {
    return signaller->Signal(fence, value);
}

__forceinline GpuStatus gpu_fence_notify(GpuFence* fence, u64 value, Waitable event) {
    return fence->SetEventOnCompletion(value, event);
}

// ---- Surface accessors -----------------------------------------------------------------------------------

__forceinline GpuRtv* gpu_surface_target(const GpuSurface* surface) {
    return surface->target;
}

__forceinline GpuTexture* gpu_surface_target_texture(const GpuSurface* surface) {
    ID3D11Resource* resource = nullptr;
    surface->target->GetResource(&resource);
    ID3D11Texture2D* texture = nullptr;
    resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    com_release(resource);
    if (texture)
        texture->Release();
    return texture;
}

__forceinline void gpu_surface_set_target(GpuSurface* surface, GpuRtv* target) {
    surface->target = target;
}

__forceinline u32 gpu_surface_width(const GpuSurface* surface) {
    return surface->buffer_width;
}

__forceinline u32 gpu_surface_height(const GpuSurface* surface) {
    return surface->buffer_height;
}

// The swap chain's back buffers are B8G8R8A8.
__forceinline GpuFormat gpu_surface_format(const GpuSurface*) {
    return GpuFormat::bgra8;
}

__forceinline bool gpu_surface_valid(const GpuSurface* surface) {
    return surface->swap_chain != nullptr;
}

__forceinline Waitable gpu_frame_clock_waitable(const GpuSurface* surface) {
    return surface->frame_latency_waitable;
}

} // namespace maicord
