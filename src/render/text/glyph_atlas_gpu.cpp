#include "maicord/render/text/glyph_atlas_gpu.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/render/d3d11/shader_bytecode.hpp"

#include "maicord/sys/d3d11.hpp"
#include "maicord/render/text/glyph_atlas.hpp"

namespace maicord {
namespace {

struct GlyphRasterJob {
    u32 origin,extent,count_fill,offset_padding;
    float pixels_per_em,outline_origin_x,outline_origin_y;
};
static_assert(sizeof(GlyphPathCommand)==28);
static_assert(sizeof(GlyphRasterJob)==sizeof(GlyphPathCommand));

constexpr GlyphAtlasGpuResult gpu_error(GlyphAtlasGpuError error,
                                        HRESULT native_error = S_OK) noexcept {
    return GlyphAtlasGpuResult{native_error, error};
}

bool bounded_float(float value, float limit) noexcept {
    return value == value && value >= -limit && value <= limit;
}

// A buffer is made at start and each time a batch outgrows it: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
HRESULT reserve_commands(GlyphAtlasGpu* gpu,ID3D11Device* device,u32 required) noexcept {
    if(required<=gpu->command_committed)return S_OK;
    const u32 capacity=minimum<u32>(gpu->command_capacity,maximum(required,maximum<u32>(128,static_cast<u32>(gpu->command_committed)*2)));
    D3D11_BUFFER_DESC description{};
    description.ByteWidth=glyph_raster_header_bytes+capacity*sizeof(GlyphPathCommand);
    description.Usage=D3D11_USAGE_DYNAMIC;
    description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    description.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ID3D11Buffer* buffer=nullptr;ID3D11ShaderResourceView* view=nullptr;
    HRESULT result=device->CreateBuffer(&description,nullptr,&buffer);
    if(SUCCEEDED(result)){
        D3D11_SHADER_RESOURCE_VIEW_DESC descriptor{};
        descriptor.Format=DXGI_FORMAT_R32_TYPELESS;descriptor.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        descriptor.BufferEx.NumElements=description.ByteWidth/4;descriptor.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        result=device->CreateShaderResourceView(buffer,&descriptor,&view);
    }
    if(FAILED(result)){com_release(view);com_release(buffer);return result;}
    // Pending GPU work retains its resources. Publish only a complete pair;
    // the following WRITE_DISCARD upload replaces all command contents.
    com_release(gpu->command_view);com_release(gpu->command_buffer);
    gpu->command_buffer=buffer;gpu->command_view=view;gpu->command_committed=static_cast<u16>(capacity);
    return S_OK;
}
#pragma clang attribute pop

} // namespace

// Device setup, once per atlas: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
GlyphAtlasGpuResult glyph_atlas_gpu_initialize(GlyphAtlasGpu* gpu, ID3D11Device* device,
                                               u16 width, u16 height,
                                               u32 command_capacity) noexcept {
    if (!gpu || !device || width == 0 || height == 0 || command_capacity == 0 ||
        command_capacity > 65'535) {
        return gpu_error(GlyphAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    reset_value<GlyphAtlasGpu>(gpu);
    gpu->width = width;
    gpu->height = height;
    gpu->command_capacity = static_cast<u16>(command_capacity);

    UINT format_support = 0;
    HRESULT result = device->CheckFormatSupport(DXGI_FORMAT_R8_UNORM, &format_support);
    constexpr UINT required_support = D3D11_FORMAT_SUPPORT_TEXTURE2D |
                                      D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                                      D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if (FAILED(result) || (format_support & required_support) != required_support) {
        glyph_atlas_gpu_release(gpu);
        return gpu_error(GlyphAtlasGpuError::unsupported_format, result);
    }

    D3D11_TEXTURE2D_DESC texture_description{};
    texture_description.Width = width;
    texture_description.Height = height;
    texture_description.MipLevels = 1;
    texture_description.ArraySize = 1;
    texture_description.Format = DXGI_FORMAT_R8_UNORM;
    texture_description.SampleDesc.Count = 1;
    texture_description.Usage = D3D11_USAGE_DEFAULT;
    texture_description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    result = device->CreateTexture2D(&texture_description, nullptr, &gpu->texture);
    if (SUCCEEDED(result)) {
        result = device->CreateShaderResourceView(gpu->texture, nullptr, &gpu->texture_view);
    }
    if (SUCCEEDED(result)) {
        result = device->CreateUnorderedAccessView(gpu->texture, nullptr, &gpu->texture_uav);
    }

    if (SUCCEEDED(result)) {
        result = reserve_commands(gpu,device,minimum<u32>(128,command_capacity));
    }

    if (SUCCEEDED(result)) {
        result = shader_create_compute(device, ShaderId::glyph_compute, &gpu->raster_shader);
    }
    if (SUCCEEDED(result)) {
        // Unstroked text runs this variant; a creation failure keeps the plain shader.
        shader_create_compute(device, ShaderId::glyph_culled_compute, &gpu->culled_shader);
    }
    if (FAILED(result)) {
        glyph_atlas_gpu_release(gpu);
        return gpu_error(GlyphAtlasGpuError::create_resource, result);
    }
    return gpu_error(GlyphAtlasGpuError::none);
}
#pragma clang attribute pop

GlyphAtlasGpuResult GlyphRasterBatch::add(const GlyphOutline& outline,const GlyphRasterBounds& bounds,
    const GlyphAtlasAllocation& allocation,float pixels_per_em,u16 allocation_padding,float stroke_half_width,
    float pen_offset) noexcept {
    auto* gpu=owner;
    if(failed)return gpu_error(GlyphAtlasGpuError::upload,E_FAIL);
    if (!gpu || !context || !gpu->texture_uav || !gpu->raster_shader || !outline.commands ||
        outline.count == 0 || !outline.has_bounds || !allocation.ok() ||
        allocation.width != bounds.width || allocation.height != bounds.height ||
        allocation.x < allocation_padding || allocation.y < allocation_padding ||
        static_cast<u32>(allocation.x) + allocation.width + allocation_padding > gpu->width ||
        static_cast<u32>(allocation.y) + allocation.height + allocation_padding > gpu->height ||
        !bounded_float(pixels_per_em, 1024.0f) || pixels_per_em <= 0.0f ||
        !(stroke_half_width >= 0.0f && stroke_half_width < 128.0f)) {
        return gpu_error(GlyphAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    if (outline.count > gpu->command_capacity)
        return gpu_error(GlyphAtlasGpuError::command_capacity);

    const bool short_of_commands=count&&outline.count>gpu->command_committed-cursor;
    bool close=count&&(count==glyph_raster_batch_limit||short_of_commands);
    const u32 x=allocation.x-allocation_padding,y=allocation.y-allocation_padding;
    const u32 width=allocation.width+2*allocation_padding,height=allocation.height+2*allocation_padding;
    for(u32 i=0;i<count&&!close;++i){const auto* j=jobs[i];
        const u32 jx=j[0]&65535,jy=j[0]>>16,jw=j[1]&65535,jh=j[1]>>16;
        close=x<jx+jw&&jx<x+width&&y<jy+jh&&jy<y+height;
    }
    if(close&&!finish())return gpu_error(GlyphAtlasGpuError::upload,E_FAIL);
    // A buffer that held less than a batch doubles (the dispatch just issued
    // keeps the old one alive) so later batches carry more jobs; an outline
    // larger than the buffer always grows it.
    const bool undersized=short_of_commands&&gpu->command_committed<gpu->command_capacity;
    const u32 required=undersized?maximum<u32>(outline.count,static_cast<u32>(gpu->command_committed)+1u):outline.count;
    if(required>gpu->command_committed){
        ID3D11Device* device=nullptr;context->GetDevice(&device);
        const HRESULT grown=reserve_commands(gpu,device,required);com_release(device);
        if(FAILED(grown))return gpu_error(GlyphAtlasGpuError::create_resource,grown);
    }
    if(!mapped){D3D11_MAPPED_SUBRESOURCE upload{};
        const HRESULT result=context->Map(gpu->command_buffer,0,D3D11_MAP_WRITE_DISCARD,0,&upload);
        if(FAILED(result)){failed=true;return gpu_error(GlyphAtlasGpuError::upload,result);}
        mapped=static_cast<u8*>(upload.pData);
    }
    CopyMemory(mapped+glyph_raster_header_bytes+static_cast<usize>(cursor)*sizeof(GlyphPathCommand),outline.commands,
               static_cast<usize>(outline.count)*sizeof(GlyphPathCommand));
    const GlyphRasterJob constants{
        x|(y<<16),width|(height<<16),outline.count|(outline.fill_rule==GlyphFillRule::winding?1u<<16:0)|
            (static_cast<u32>(stroke_half_width*256.0f+0.5f)<<17),
        cursor|(static_cast<u32>(allocation_padding)<<16),pixels_per_em,(bounds.left-pen_offset)/pixels_per_em,
        bounds.top/pixels_per_em};
    copy_bytes(jobs[count++],&constants,sizeof(constants));cursor+=outline.count;
    groups_x=maximum(groups_x,(width+7)/8);groups_y=maximum(groups_y,(height+7)/8);
    large=large||width>=glyph_raster_culling_extent||height>=glyph_raster_culling_extent;
    peak_commands=maximum(peak_commands,outline.count);
    stroked=stroked||stroke_half_width>0.0f;
    return gpu_error(GlyphAtlasGpuError::none);
}
void GlyphRasterBatch::cancel() noexcept {
    if(mapped)context->Unmap(owner->command_buffer,0);
    mapped=nullptr;count=cursor=groups_x=groups_y=peak_commands=0;large=stroked=false;
}
bool GlyphRasterBatch::finish() noexcept {
    if(failed){cancel();return false;}
    if(!count)return true;
    auto* gpu=owner;copy_bytes(mapped,jobs,count*sizeof(GlyphRasterJob));
    context->Unmap(gpu->command_buffer,0);mapped=nullptr;
    ID3D11ComputeShader* shader=gpu->raster_shader;
    if(!stroked&&gpu->culled_shader&&(large||peak_commands>=glyph_raster_culling_commands))shader=gpu->culled_shader;
    context->CSSetShader(shader,nullptr,0);
    context->CSSetShaderResources(0,1,&gpu->command_view);
    context->CSSetUnorderedAccessViews(0,1,&gpu->texture_uav,nullptr);
    context->Dispatch(groups_x,groups_y,count);
    ID3D11UnorderedAccessView* uav=nullptr;ID3D11ShaderResourceView* view=nullptr;
    context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context->CSSetShaderResources(0,1,&view);
    context->CSSetShader(nullptr,nullptr,0);
    ++gpu->dispatch_count;gpu->raster_count+=count;gpu->dispatched_commands+=cursor;
    for(u32 i=0;i<count;++i)gpu->dispatched_pixels+=static_cast<u64>(jobs[i][1]&65535)*(jobs[i][1]>>16);
    count=cursor=groups_x=groups_y=peak_commands=0;large=stroked=false;return true;
}

// Growth and trim run once per atlas step or idle trim, release at exit: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
HRESULT glyph_atlas_texture_resize(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D** texture,
                                   ID3D11ShaderResourceView** view, ID3D11UnorderedAccessView** uav, u16 height,
                                   ConstSlice<GlyphAtlasRowMove> moves) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    (*texture)->GetDesc(&description);
    const UINT old_height = description.Height;
    description.Height = height;
    ID3D11Texture2D* grown = nullptr;
    ID3D11ShaderResourceView* grown_view = nullptr;
    ID3D11UnorderedAccessView* grown_uav = nullptr;
    HRESULT result = device->CreateTexture2D(&description, nullptr, &grown);
    if (SUCCEEDED(result)) {
        result = device->CreateShaderResourceView(grown, nullptr, &grown_view);
    }
    if (SUCCEEDED(result)) {
        result = device->CreateUnorderedAccessView(grown, nullptr, &grown_uav);
    }
    if (FAILED(result)) {
        com_release(grown_uav);
        com_release(grown_view);
        com_release(grown);
        return result;
    }
    // The rows both heights keep, then each trim move.
    for (u32 i = 0; i <= moves.count; ++i) {
        const GlyphAtlasRowMove move =
            i ? moves[i - 1] : GlyphAtlasRowMove{0, 0, static_cast<u16>(minimum<UINT>(old_height, height))};
        const D3D11_BOX source{0, move.source_y, 0, description.Width, static_cast<UINT>(move.source_y) + move.height, 1};
        context->CopySubresourceRegion(grown, 0, 0, move.destination_y, 0, *texture, 0, &source);
    }
    com_release(*uav);
    com_release(*view);
    com_release(*texture);
    *texture = grown;
    *view = grown_view;
    *uav = grown_uav;
    return S_OK;
}

void glyph_atlas_gpu_release(GlyphAtlasGpu* gpu) noexcept {
    if (!gpu) {
        return;
    }
    com_release(gpu->raster_shader);
    com_release(gpu->culled_shader);
    com_release(gpu->command_view);
    com_release(gpu->command_buffer);
    com_release(gpu->texture_uav);
    com_release(gpu->texture_view);
    com_release(gpu->texture);
    gpu->width = 0;
    gpu->height = 0;
    gpu->command_capacity = 0;
    gpu->command_committed = 0;
}
#pragma clang attribute pop

} // namespace maicord
