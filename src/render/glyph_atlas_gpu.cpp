#include "maicord/render/glyph_atlas_gpu.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/render/shader_bytecode.hpp"

#include "maicord/sys/d3d11.hpp"
#include "maicord/render/glyph_atlas.hpp"

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

} // namespace

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
    if (FAILED(result)) {
        glyph_atlas_gpu_release(gpu);
        return gpu_error(GlyphAtlasGpuError::create_resource, result);
    }
    return gpu_error(GlyphAtlasGpuError::none);
}

GlyphAtlasGpuResult GlyphRasterBatch::add(const GlyphOutline& outline,const GlyphRasterBounds& bounds,
    const GlyphAtlasAllocation& allocation,float pixels_per_em,u16 allocation_padding,float stroke_half_width) noexcept {
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
        if (gpu) {
            ++gpu->rejected;
        }
        return gpu_error(GlyphAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    if (outline.count > gpu->command_capacity) {
        ++gpu->rejected;
        return gpu_error(GlyphAtlasGpuError::command_capacity);
    }

    bool close=count&&(count==glyph_raster_batch_limit||outline.count>gpu->command_committed-cursor);
    const u32 x=allocation.x-allocation_padding,y=allocation.y-allocation_padding;
    const u32 width=allocation.width+2*allocation_padding,height=allocation.height+2*allocation_padding;
    for(u32 i=0;i<count&&!close;++i){const auto* j=jobs[i];
        const u32 jx=j[0]&65535,jy=j[0]>>16,jw=j[1]&65535,jh=j[1]>>16;
        close=x<jx+jw&&jx<x+width&&y<jy+jh&&jy<y+height;
    }
    if(close&&!finish())return gpu_error(GlyphAtlasGpuError::upload,E_FAIL);
    if(outline.count>gpu->command_committed){
        ID3D11Device* device=nullptr;context->GetDevice(&device);
        const HRESULT grown=reserve_commands(gpu,device,outline.count);com_release(device);
        if(FAILED(grown)){++gpu->rejected;return gpu_error(GlyphAtlasGpuError::create_resource,grown);}
    }
    if(!mapped){D3D11_MAPPED_SUBRESOURCE upload{};
        const HRESULT result=context->Map(gpu->command_buffer,0,D3D11_MAP_WRITE_DISCARD,0,&upload);
        if(FAILED(result)){++gpu->rejected;failed=true;return gpu_error(GlyphAtlasGpuError::upload,result);}
        mapped=static_cast<u8*>(upload.pData);
    }
    CopyMemory(mapped+glyph_raster_header_bytes+static_cast<usize>(cursor)*sizeof(GlyphPathCommand),outline.commands,
               static_cast<usize>(outline.count)*sizeof(GlyphPathCommand));
    const GlyphRasterJob constants{
        x|(y<<16),width|(height<<16),outline.count|(outline.fill_rule==GlyphFillRule::winding?1u<<16:0)|
            (static_cast<u32>(stroke_half_width*256.0f+0.5f)<<17),
        cursor|(static_cast<u32>(allocation_padding)<<16),pixels_per_em,bounds.left/pixels_per_em,bounds.top/pixels_per_em};
    copy_bytes(jobs[count++],&constants,sizeof(constants));cursor+=outline.count;
    groups_x=maximum(groups_x,(width+7)/8);groups_y=maximum(groups_y,(height+7)/8);
    large=large||width>=glyph_raster_culling_extent||height>=glyph_raster_culling_extent;
    stroked=stroked||stroke_half_width>0.0f;
    return gpu_error(GlyphAtlasGpuError::none);
}
void GlyphRasterBatch::cancel() noexcept {
    if(mapped)context->Unmap(owner->command_buffer,0);
    mapped=nullptr;count=cursor=groups_x=groups_y=0;large=stroked=false;
}
bool GlyphRasterBatch::finish() noexcept {
    if(failed){cancel();return false;}
    if(!count)return true;
    auto* gpu=owner;copy_bytes(mapped,jobs,count*sizeof(GlyphRasterJob));
    context->Unmap(gpu->command_buffer,0);mapped=nullptr;
    ID3D11ComputeShader* shader=gpu->raster_shader;
    if(large&&!stroked){
        if(!gpu->culled_shader){
            ID3D11Device* device=nullptr;context->GetDevice(&device);
            if(device)shader_create_compute(device,ShaderId::glyph_culled_compute,&gpu->culled_shader);
            com_release(device);
        }
        if(gpu->culled_shader)shader=gpu->culled_shader; // Creation failure keeps the plain shader.
    }
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
    count=cursor=groups_x=groups_y=0;large=stroked=false;return true;
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

} // namespace maicord
