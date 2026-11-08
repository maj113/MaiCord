#include "maicord/render/text/glyph_atlas_gpu.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/gpu/api.hpp"

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
                                        GpuStatus native_error = kGpuOk) noexcept {
    return GlyphAtlasGpuResult{native_error, error};
}

bool bounded_float(float value, float limit) noexcept {
    return value == value && value >= -limit && value <= limit;
}

// A buffer is made at start and each time a batch outgrows it: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
GpuStatus reserve_commands(GlyphAtlasGpu* gpu,GpuDevice* device,u32 required) noexcept {
    if(required<=gpu->command_committed)return kGpuOk;
    const u32 capacity=minimum<u32>(gpu->command_capacity,maximum(required,maximum<u32>(128,static_cast<u32>(gpu->command_committed)*2)));
    const u32 bytes=glyph_raster_header_bytes+capacity*sizeof(GlyphPathCommand);
    GpuBuffer* buffer=nullptr;GpuSrv* view=nullptr;
    GpuStatus result=gpu_buffer_create(device,{bytes,GpuUsage::dynamic,gpu_bind_srv,true},nullptr,&buffer);
    if(gpu_ok(result))result=gpu_srv_create(device,buffer,{GpuViewKind::buffer_raw,GpuFormat::r32_uint,bytes/4},&view);
    if(!gpu_ok(result)){gpu_release(view);gpu_release(buffer);return result;}
    // Pending GPU work retains its resources. Publish only a complete pair;
    // the following WRITE_DISCARD upload replaces all command contents.
    gpu_release(gpu->command_view);gpu_release(gpu->command_buffer);
    gpu->command_buffer=buffer;gpu->command_view=view;gpu->command_committed=static_cast<u16>(capacity);
    return kGpuOk;
}
#pragma clang attribute pop

} // namespace

// Device setup, once per atlas: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
namespace {

// The command buffer and raster shaders, shared by both kinds of atlas.
GlyphAtlasGpuResult create_raster_resources(GlyphAtlasGpu* gpu, GpuDevice* device) noexcept {
    GpuStatus result = reserve_commands(gpu, device, minimum<u32>(128, gpu->command_capacity));
    if (SUCCEEDED(result)) {
        result = gpu_compute_shader_create(device, ShaderId::glyph_compute, &gpu->raster_shader);
    }
    if (SUCCEEDED(result)) {
        // Unstroked text runs this variant; a creation failure keeps the plain shader.
        gpu_compute_shader_create(device, ShaderId::glyph_culled_compute, &gpu->culled_shader);
    }
    if (FAILED(result)) {
        glyph_atlas_gpu_release(gpu);
        return gpu_error(GlyphAtlasGpuError::create_resource, result);
    }
    return gpu_error(GlyphAtlasGpuError::none);
}

GlyphAtlasGpuResult start(GlyphAtlasGpu* gpu, GpuDevice* device, u16 width, u16 height,
                          u32 command_capacity) noexcept {
    if (!gpu || !device || width == 0 || height == 0 || command_capacity == 0 || command_capacity > 65'535) {
        return gpu_error(GlyphAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    reset_value<GlyphAtlasGpu>(gpu);
    gpu->width = width;
    gpu->height = height;
    gpu->command_capacity = static_cast<u16>(command_capacity);
    const GpuStatus result = gpu_format_check(device, GpuFormat::r8, static_cast<u8>(gpu_bind_srv | gpu_bind_uav));
    return gpu_ok(result) ? gpu_error(GlyphAtlasGpuError::none)
                          : gpu_error(GlyphAtlasGpuError::unsupported_format, result);
}

} // namespace

GlyphAtlasGpuResult glyph_atlas_gpu_initialize(GlyphAtlasGpu* gpu, GpuDevice* device,
                                               u16 width, u16 height,
                                               u32 command_capacity) noexcept {
    const GlyphAtlasGpuResult started = start(gpu, device, width, height, command_capacity);
    if (!started.ok()) {
        return started;
    }
    GpuStatus result =
        gpu_texture_create(device, {width, height, GpuFormat::r8, GpuUsage::default_, gpu_bind_srv | gpu_bind_uav},
                           nullptr, &gpu->texture);
    if (SUCCEEDED(result)) {
        result = gpu_srv_create(device, gpu->texture, {GpuViewKind::texture, GpuFormat::r8}, &gpu->texture_view);
    }
    if (SUCCEEDED(result)) {
        result = gpu_uav_create(device, gpu->texture, &gpu->texture_uav);
    }
    if (FAILED(result)) {
        glyph_atlas_gpu_release(gpu);
        return gpu_error(GlyphAtlasGpuError::create_resource, result);
    }
    return create_raster_resources(gpu, device);
}

GlyphAtlasGpuResult glyph_atlas_gpu_initialize_paged(GlyphAtlasGpu* gpu, GpuDevice* device, u16 width, u16 page_rows,
                                                     u8 pages, u32 command_capacity) noexcept {
    const GlyphAtlasGpuResult started =
        start(gpu, device, width, static_cast<u16>(page_rows * pages), command_capacity);
    if (!started.ok()) {
        return started;
    }
    const GpuStatus result = gpu_paged_texture_create(
        device, {width, page_rows, pages, GpuFormat::r8, gpu_bind_srv | gpu_bind_uav}, &gpu->surface);
    if (FAILED(result)) {
        glyph_atlas_gpu_release(gpu);
        return gpu_error(GlyphAtlasGpuError::create_resource, result);
    }
    gpu->texture_view = gpu_paged_texture_view(gpu->surface);
    return create_raster_resources(gpu, device);
}
#pragma clang attribute pop

GlyphAtlasGpuResult GlyphRasterBatch::add(const GlyphOutline& outline,const GlyphRasterBounds& bounds,
    const GlyphAtlasAllocation& allocation,float pixels_per_em,u16 allocation_padding,float stroke_half_width,
    float pen_offset) noexcept {
    auto* gpu=owner;
    if(failed)return gpu_error(GlyphAtlasGpuError::upload,E_FAIL);
    const GpuPage page=gpu&&gpu->surface?gpu_paged_texture_page(gpu->surface,allocation.y)
                                         :GpuPage{nullptr,gpu?gpu->texture_uav:nullptr,nullptr,0};
    if (!gpu || !context || !page.uav || !gpu->raster_shader || !outline.commands ||
        outline.count == 0 || !outline.has_bounds || !allocation.ok() ||
        allocation.width != bounds.width || allocation.height != bounds.height ||
        allocation.x < allocation_padding || allocation.y < allocation_padding ||
        static_cast<u32>(allocation.x) + allocation.width + allocation_padding > gpu->width ||
        static_cast<u32>(allocation.y) + allocation.height + allocation_padding > gpu->height ||
        !bounded_float(pixels_per_em, 1024.0f) || pixels_per_em <= 0.0f ||
        !(stroke_half_width >= 0.0f && stroke_half_width < 128.0f) ||
        // The raster with its gutter lies in backed rows: no shader touches an unbacked tile.
        (gpu->surface && !gpu_paged_texture_backed(gpu->surface, allocation.y - allocation_padding,
                                                   allocation.height + 2u * allocation_padding))) {
        return gpu_error(GlyphAtlasGpuError::invalid_input, E_INVALIDARG);
    }
    if (outline.count > gpu->command_capacity)
        return gpu_error(GlyphAtlasGpuError::command_capacity);

    const bool short_of_commands=count&&outline.count>gpu->command_committed-cursor;
    bool close=count&&(count==glyph_raster_batch_limit||short_of_commands||page.uav!=target);
    // The job in its page's rows.
    const u32 x=allocation.x-allocation_padding,y=allocation.y-allocation_padding-page.first_row;
    const u32 width=allocation.width+2*allocation_padding,height=allocation.height+2*allocation_padding;
    for(u32 i=0;i<count&&!close;++i){const auto* j=jobs[i];
        const u32 jx=j[0]&65535,jy=j[0]>>16,jw=j[1]&65535,jh=j[1]>>16;
        close=x<jx+jw&&jx<x+width&&y<jy+jh&&jy<y+height;
    }
    if(close&&!finish())return gpu_error(GlyphAtlasGpuError::upload,E_FAIL);
    target=page.uav;
    // A buffer that held less than a batch doubles (the dispatch just issued
    // keeps the old one alive) so later batches carry more jobs; an outline
    // larger than the buffer always grows it.
    const bool undersized=short_of_commands&&gpu->command_committed<gpu->command_capacity;
    const u32 required=undersized?maximum<u32>(outline.count,static_cast<u32>(gpu->command_committed)+1u):outline.count;
    if(required>gpu->command_committed){
        GpuDevice* device=gpu_context_device(context);
        const GpuStatus grown=reserve_commands(gpu,device,required);
        if(FAILED(grown))return gpu_error(GlyphAtlasGpuError::create_resource,grown);
    }
    if(!mapped){GpuStatus result=kGpuOk;
        void* upload=gpu_map(context,gpu->command_buffer,GpuMap::discard,&result);
        if(!upload){failed=true;return gpu_error(GlyphAtlasGpuError::upload,result);}
        mapped=static_cast<u8*>(upload);
    }
    // Each end_closed goes up as a line back to its contour's first point, so the shaders close a contour through
    // the same line path as every other edge and keep no contour start. The shaders' separate closing edge was lost
    // on Intel's Windows driver (UHD 630, issue #11): an L lost its bar, an n its right stem, a d filled in.
    auto* destination=reinterpret_cast<GlyphPathCommand*>(mapped+glyph_raster_header_bytes)+cursor;
    float start_x=0,start_y=0;
    for(u32 i=0;i<outline.count;++i){
        GlyphPathCommand command=outline.commands[i];
        if(command.kind==GlyphPathCommandKind::begin_filled||command.kind==GlyphPathCommandKind::begin_hollow){
            start_x=command.x1;start_y=command.y1;
        }else if(command.kind==GlyphPathCommandKind::end_closed){
            command={start_x,start_y,0,0,0,0,GlyphPathCommandKind::line,{}};
        }
        destination[i]=command;
    }
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
    if(mapped)gpu_unmap(context,owner->command_buffer);
    mapped=nullptr;count=cursor=groups_x=groups_y=peak_commands=0;large=stroked=false;
}
bool GlyphRasterBatch::finish() noexcept {
    if(failed){cancel();return false;}
    if(!count)return true;
    auto* gpu=owner;copy_bytes(mapped,jobs,count*sizeof(GlyphRasterJob));
    gpu_unmap(context,gpu->command_buffer);mapped=nullptr;
    GpuComputeShader* shader=gpu->raster_shader;
    if(!stroked&&gpu->culled_shader&&(large||peak_commands>=glyph_raster_culling_commands))shader=gpu->culled_shader;
    gpu_set_shader(context,shader);
    gpu_set_srvs(context,GpuStage::compute,0,1,&gpu->command_view);
    gpu_set_uavs(context,0,1,&target);
    gpu_dispatch(context,groups_x,groups_y,count);
    GpuUav* uav=nullptr;GpuSrv* view=nullptr;
    gpu_set_uavs(context,0,1,&uav);
    gpu_set_srvs(context,GpuStage::compute,0,1,&view);
    gpu_set_shader(context,static_cast<GpuComputeShader*>(nullptr));
    ++gpu->dispatch_count;gpu->raster_count+=count;gpu->dispatched_commands+=cursor;
    for(u32 i=0;i<count;++i)gpu->dispatched_pixels+=static_cast<u64>(jobs[i][1]&65535)*(jobs[i][1]>>16);
    count=cursor=groups_x=groups_y=peak_commands=0;large=stroked=false;return true;
}

// Release at exit: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

void glyph_atlas_gpu_release(GlyphAtlasGpu* gpu) noexcept {
    if (!gpu) {
        return;
    }
    gpu_release(gpu->raster_shader);
    gpu_release(gpu->culled_shader);
    gpu_release(gpu->command_view);
    gpu_release(gpu->command_buffer);
    gpu_release(gpu->texture_uav);
    if (gpu->surface) {
        gpu->texture_view = nullptr; // The surface's own view.
        gpu_paged_texture_release(&gpu->surface);
    }
    gpu_release(gpu->texture_view);
    gpu_release(gpu->texture);
    gpu->width = 0;
    gpu->height = 0;
    gpu->command_capacity = 0;
    gpu->command_committed = 0;
}
#pragma clang attribute pop

} // namespace maicord
