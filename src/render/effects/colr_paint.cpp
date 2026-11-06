#include "maicord/render/effects/colr_paint.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"

#include "maicord/render/text/glyph_atlas.hpp"

namespace maicord {
namespace {
// Resources are created once per atlas: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// A one-page atlas of extent x height, backed whole (`surface`), and the shared job constants.
bool initialize(ColrPaintGpu* g, GpuContext* context, GpuDevice* device, u16 extent, u16 height,
                bool surface) noexcept {
    g->append_upload = gpu_caps(device).map_no_overwrite_on_srv;
    if ((surface && !g->surface &&
         (FAILED(gpu_paged_texture_create(device, {extent, height, 1, GpuFormat::rgba8, gpu_bind_srv | gpu_bind_uav},
                                          &g->surface)) ||
          FAILED(gpu_paged_texture_commit(context, g->surface, 0, height)))) ||
        (!g->constants &&
         FAILED(gpu_buffer_create(device, {sizeof(ColrPaintJob) * kColrPaintBatchJobs, GpuUsage::dynamic, gpu_bind_constant, false},
                                  nullptr, &g->constants)))) {
        colr_paint_gpu_release(g);
        return false;
    }
    if (g->surface && !g->width) {
        g->view = gpu_paged_texture_view(g->surface);
        g->width = extent;
        g->height = height;
    }
    return true;
}
#pragma clang attribute pop
// Only an already accumulated destination followed by one shaped source may
// use the stack-free kernel. Nested groups, masks and other composite modes
// keep the general interpreter and its evaluation order.
u32 layered_node_count(const ColrPaintProgram& p) noexcept {
    u32 at=p.first, depth=0, clipped=0;
    for(u32 i=0;i<p.nodes;++i) {
        if(at>p.storage.offset || sizeof(ColrPaintOp)>p.storage.offset-at)return 0;
        const auto& n=*reinterpret_cast<const ColrPaintOp*>(p.storage.base+at);
        const usize bytes=n.kind==8?sizeof(ColrSolidPaintNode):
            n.kind==6?sizeof(ColrPaintOp):sizeof(ColrPaintNode);
        if(bytes>p.storage.offset-at)return 0;
        if(n.kind>=8&&n.kind<=11) {
            if(clipped||depth==2)return 0;
            if(n.reserved>p.storage.offset || sizeof(ColrMask)>p.storage.offset-n.reserved)return 0;
            ++depth;
        } else if(n.kind==6) {
            if(clipped||depth!=2||n.mode!=3||n.count!=1)return 0;
            depth=1;
        } else if(n.kind==5) {
            if(depth!=1)return 0;
            ++clipped;
        } else return 0;
        at=n.next;
    }
    // Each extra source has one implicit composite; clip nodes remain in the plan.
    return depth==1?static_cast<u32>((u64{p.nodes}+clipped+1)/2):0;
}
u32 layer_plan_capacity(const ColrPaintProgram& p,const GlyphRasterBounds& b,u16 padding,u32 layer_nodes) noexcept {
    const u32 columns=(b.width+2*padding+7)/8,rows=(b.height+2*padding+7)/8;
    // Bound preprocessing and upload growth, including large public media paints.
    if(columns*rows>1024 || static_cast<u64>(columns)*rows*(p.nodes+2)>131072)return 0;
    return columns*rows*(layer_nodes+2)*4;
}
u32 write_layer_plan(const ColrPaintProgram& p,const GlyphRasterBounds& b,u16 padding,u8* target) noexcept {
    const u32 columns=(b.width+2*padding+7)/8,rows=(b.height+2*padding+7)/8;
    const u32 start=static_cast<u32>(p.storage.offset),table_bytes=columns*rows*8;
    auto* table=reinterpret_cast<u32*>(target+start);
    auto* indices=reinterpret_cast<u32*>(target+start+table_bytes);
    u32 count=0;
    for(u32 y=0;y<rows;++y)for(u32 x=0;x<columns;++x) {
        const u32 tile=y*columns+x,first=count;
        const i64 left=i64{b.left}-padding+x*8,top=i64{b.top}-padding+y*8;
        u32 at=p.first;
        for(u32 i=0;i<p.nodes;++i) {
            const auto& n=*reinterpret_cast<const ColrPaintOp*>(p.storage.base+at);
            bool include=n.kind==5;
            if(n.kind>=8&&n.kind<=11) {
                const auto& mask=*reinterpret_cast<const ColrMask*>(p.storage.base+n.reserved);
                include=mask.left<left+8 && mask.top<top+8 &&
                    i64{mask.left}+mask.width>left && i64{mask.top}+mask.height>top;
            }
            if(include)indices[count++]=at;
            at=n.next;
        }
        table[tile*2]=start+table_bytes+first*4;
        table[tile*2+1]=count-first;
    }
    return table_bytes+count*4;
}
// The layer plan above is a per-glyph loop and stays optimized for speed; binding,
// resource creation, the queue and its dispatch are D3D calls: compiled for size
// (the job-extent loop stays scalar).
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
void unbind_colr(GpuContext* context) noexcept {
    GpuUav* uav=nullptr;
    GpuSrv* views[2]{};
    GpuBuffer* buffer=nullptr;
    gpu_set_uavs(context,0,1,&uav);
    gpu_set_srvs(context,GpuStage::compute,0,2,views);
    gpu_set_constants(context,GpuStage::compute,0,1,&buffer);
    gpu_set_shader(context,static_cast<GpuComputeShader*>(nullptr));
}
GpuComputeShader* ensure_colr(ColrPaintGpu* g,GpuContext* context,u32 bytes,bool general,u16 extent,u16 height,bool external_target=false,bool layers=false) noexcept {
    auto** shader = layers ? &g->layers_shader : general ? &g->wide_shader : &g->shader;
    const bool initialize_resources = !g->constants || (!external_target && !g->surface);
    const bool resize = initialize_resources || bytes > g->capacity || !*shader;
    GpuDevice* device = nullptr;
    if (resize)
        device = gpu_context_device(context);
    bool ok = !resize || device;
    if (ok && initialize_resources)
        ok = initialize(g, context, device, extent, height, !external_target);
    if (ok && bytes > g->capacity) {
        // Retain upload slack without doubling a large paint's entire buffer.
        // Programs are bounded by the eight-MiB compilation arena.
        const u32 quantum = 256 * 1024;
        const u32 capacity = (bytes + quantum - 1) & ~(quantum - 1);
        GpuBuffer* buffer = nullptr;
        GpuSrv* view = nullptr;
        ok = SUCCEEDED(gpu_buffer_create(device, {capacity, GpuUsage::dynamic, gpu_bind_srv, true}, nullptr, &buffer)) &&
             SUCCEEDED(gpu_srv_create(device, buffer, {GpuViewKind::buffer_raw, GpuFormat::r32_uint, capacity / 4}, &view));
        if (ok) {
            gpu_release(g->program_view);
            gpu_release(g->program);
            g->program = buffer;
            g->program_view = view;
            g->capacity = capacity;
            g->upload_cursor = 0;
        } else {
            gpu_release(view);
            gpu_release(buffer);
        }
    }
    if (ok && !*shader)
        ok = SUCCEEDED(gpu_compute_shader_create(device, layers ? ShaderId::colr_layers_compute :
            general ? ShaderId::colr_wide_compute : ShaderId::colr_compute, shader));
    return ok ? *shader : nullptr;
}
// One dispatch for `count` jobs: group z selects the job, and groups past a
// smaller job's extent return at once.
bool dispatch_jobs(ColrPaintGpu* g, GpuContext* context, GpuComputeShader* shader,
                   const ColrPaintJob* jobs, u32 count, GpuUav* output) noexcept {
    void* mapped = gpu_map(context, g->constants, GpuMap::discard, nullptr);
    if (!mapped)
        return false;
    CopyMemory(mapped, jobs, count * sizeof(ColrPaintJob));
    gpu_unmap(context, g->constants);
    u32 columns = 0, rows = 0;
    for (u32 i = 0; i < count; ++i) {
        columns = maximum(columns, (jobs[i].width + 7) / 8);
        rows = maximum(rows, (jobs[i].height + 7) / 8);
    }
    gpu_set_constants(context, GpuStage::compute, 0, 1, &g->constants);
    gpu_set_srvs(context, GpuStage::compute, 0, 1, &g->program_view);
    gpu_set_shader(context, shader);
    gpu_set_uavs(context, 0, 1, &output);
    gpu_dispatch(context, columns, rows, count);
    return true;
}
} // namespace
bool ColrPaintBatch::flush() noexcept {
    if (!pending)
        return true;
    const u32 count = pending;
    pending = 0;
    bound = true;
    return dispatch_jobs(gpu, context, shader, jobs, count, output);
}
void ColrPaintBatch::reset() noexcept {
    flush();
    if(bound)unbind_colr(context);
    gpu=nullptr;shader=nullptr;pending=0;bound=false;
}
#pragma clang attribute pop
static bool rasterize_colr(ColrPaintGpu* g, GpuContext* context, const ColrPaintProgram& p,
                          const GlyphRasterBounds& bounds, const GlyphAtlasAllocation& a, float scale,
                          u16 extent, u16 target_height, u16 padding, ColrPaintBatch* batch, GpuUav* target) noexcept {
    if (!g || !context || !p.storage.base || !p.nodes || p.stack != 1 || p.peak_stack > 64 ||
        p.first >= p.storage.offset || !a.ok() || a.width != bounds.width || a.height != bounds.height ||
        a.x < padding || a.y < padding || a.x + a.width + padding > extent ||
        a.y + a.height + padding > target_height || !(scale > 0.f) || scale > 1024.f ||
        (batch && batch->context != context))
        return false;
    const u32 layer_nodes = layered_node_count(p);
    const bool layers = layer_nodes != 0;
    const u32 plan_capacity=layers?layer_plan_capacity(p,bounds,padding,layer_nodes):0;
    const u32 bytes = static_cast<u32>(p.storage.offset)+plan_capacity;
    const bool general = !layers && p.peak_stack > 8;
    // Without a target the job writes the page of the atlas holding it, in that page's rows.
    const GpuPage page = target ? GpuPage{nullptr, target, nullptr, 0}
                         : g->surface ? gpu_paged_texture_page(g->surface, a.y)
                                      : GpuPage{};
    ColrPaintJob job{static_cast<u32>(a.x - padding), static_cast<u32>(a.y - padding) - page.first_row,
                     static_cast<u32>(a.width + 2 * padding), static_cast<u32>(a.height + 2 * padding),
                     static_cast<float>(bounds.left), static_cast<float>(bounds.top), scale, p.first, padding, p.nodes, 0,
                     plan_capacity ? static_cast<u32>(p.storage.offset) : 0};
    // Queued jobs read the upload buffer and kernel as they stand: a buffer
    // rename or growth, another kernel or owner, a full queue, or a job that
    // paints over a queued job's pixels dispatches them first (jobs in one
    // dispatch run in no order).
    if (batch && batch->pending) {
        const auto* kernel = layers ? &g->layers_shader : general ? &g->wide_shader : &g->shader;
        bool flush = batch->gpu != g || batch->shader != *kernel || batch->pending == kColrPaintBatchJobs ||
                     batch->output != page.uav || !g->append_upload || bytes > g->capacity - g->upload_cursor;
        for (u32 i = 0; !flush && i < batch->pending; ++i) {
            const auto& queued = batch->jobs[i];
            flush = job.x < queued.x + queued.width && queued.x < job.x + job.width && job.y < queued.y + queued.height &&
                    queued.y < job.y + job.height;
        }
        if (flush && !batch->flush())
            return false;
    }
    auto* shader=ensure_colr(g,context,bytes,general,extent,target_height,target!=nullptr,layers);
    if(!shader)return false;
    // A one-page atlas made just now: its page.
    const GpuUav* output = page.uav ? page.uav : gpu_paged_texture_page(g->surface, a.y).uav;
    if (!output)
        return false;
    // Append disjoint ranges until full. DISCARD renames the entire resource,
    // so doing it for every tiny glyph after a large one wastes driver memory.
    if (!g->append_upload || bytes > g->capacity - g->upload_cursor)
        g->upload_cursor = 0;
    const u32 program_base = g->upload_cursor;
    void* mapped = gpu_map(context, g->program, program_base ? GpuMap::no_overwrite : GpuMap::discard, nullptr);
    if (!mapped)
        return false;
    auto* upload=static_cast<u8*>(mapped)+program_base;
    CopyMemory(upload,p.storage.base,p.storage.offset);
    const u32 plan_bytes=plan_capacity?write_layer_plan(p,bounds,padding,upload):0;
    if(plan_capacity)++g->spatial_jobs;
    gpu_unmap(context, g->program);
    g->upload_cursor = (program_base + static_cast<u32>(p.storage.offset) + plan_bytes + 15) & ~15u;
    job.program_base = program_base;
    ++g->dispatches;
    if (batch) {
        batch->jobs[batch->pending++] = job;
        batch->gpu = g;
        batch->shader = shader;
        batch->output = const_cast<GpuUav*>(output);
        return true;
    }
    const bool dispatched = dispatch_jobs(g, context, shader, &job, 1, const_cast<GpuUav*>(output));
    unbind_colr(context);
    return dispatched;
}
bool colr_paint_rasterize(ColrPaintGpu* g, GpuContext* context, const ColrPaintProgram& p,
    const GlyphRasterBounds& bounds, const GlyphAtlasAllocation& allocation, float scale,
    u16 atlas_width, u16 atlas_height, u16 padding, ColrPaintBatch* batch) noexcept {
    // An atlas made at another size cannot take it.
    if (g && g->surface && (g->width != atlas_width || g->height != atlas_height))
        return false;
    return rasterize_colr(g,context,p,bounds,allocation,scale,atlas_width,atlas_height,padding,batch,nullptr);
}
#if !defined(_WIN32)
bool colr_paint_upload_pixels(ColrPaintGpu* g, GpuContext* context, const GlyphAtlasAllocation& a, u16 atlas_width,
                              u16 atlas_height, u16 padding, const u32* pixels, Arena* scratch,
                              ColrPaintBatch* batch) noexcept {
    if (!g || !context || !pixels || !scratch || !scratch->base || !a.ok() || a.x < padding || a.y < padding ||
        a.x + a.width + padding > atlas_width || a.y + a.height + padding > atlas_height ||
        (batch && batch->context != context))
        return false;
    if (g->surface && (g->width != atlas_width || g->height != atlas_height))
        return false;
    GpuDevice* device = gpu_context_device(context);
    if (!g->surface && !initialize(g, context, device, atlas_width, atlas_height, true))
        return false;
    // Queued paint jobs write this texture too.
    if (batch && batch->pending && !batch->flush())
        return false;
    const u32 width = a.width + 2u * padding;
    const u32 height = a.height + 2u * padding;
    const ArenaMark mark = arena_mark(scratch);
    u32* padded = arena_allocate_zero<u32>(scratch, width * height);
    if (!padded)
        return false;
    for (u32 y = 0; y < a.height; ++y)
        copy_bytes(padded + (y + padding) * width + padding, pixels + y * a.width, a.width * 4u);
    GpuTexture* staged = nullptr;
    const GpuInitialData initial{padded, width * 4u};
    const bool created =
        !FAILED(gpu_texture_create(device, {width, height, GpuFormat::rgba8, GpuUsage::immutable, gpu_bind_srv}, &initial,
                                   &staged));
    const GpuPage page = gpu_paged_texture_page(g->surface, a.y);
    if (created && page.texture)
        gpu_copy_region(context, page.texture, a.x - padding, a.y - padding - page.first_row, staged,
                        {0, 0, width, height});
    gpu_release(staged);
    arena_rewind(scratch, mark);
    return created;
}
#endif
// Target bakes and release run once per image: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool colr_paint_rasterize_target(ColrPaintGpu* g, GpuContext* context,
    const ColrPaintProgram& p, const GlyphRasterBounds& bounds,
    const GlyphAtlasAllocation& allocation, float scale,
    u16 target_width, u16 target_height, u16 padding, GpuUav* target) noexcept {
    if (!target || p.storage.offset > 8 * 1024 * 1024)
        return false;
    return rasterize_colr(g,context,p,bounds,allocation,scale,target_width,target_height,padding,nullptr,target);
}
bool colr_paint_attach_surface(ColrPaintGpu* g, GpuDevice* device, const GlyphAtlasAllocator& atlas) noexcept {
    if (!g || !device)
        return false;
    if (!g->surface) {
        if (FAILED(gpu_paged_texture_create(
                device, {atlas.width, atlas.page_rows, atlas.page_count, GpuFormat::rgba8, gpu_bind_srv | gpu_bind_uav},
                &g->surface)))
            return false;
        g->view = gpu_paged_texture_view(g->surface);
        g->width = atlas.width;
        g->height = atlas.height;
    }
    return true;
}
void colr_paint_gpu_release(ColrPaintGpu* g) noexcept {
    if (!g)
        return;
    gpu_release(g->wide_shader);
    gpu_release(g->layers_shader);
    gpu_release(g->shader);
    gpu_release(g->constants);
    gpu_release(g->program_view);
    gpu_release(g->program);
    g->view = nullptr; // The surface's own view.
    gpu_paged_texture_release(&g->surface);
    reset_value(g);
}
#pragma clang attribute pop
} // namespace maicord
