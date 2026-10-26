#include "maicord/render/colr_paint.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/shader_bytecode.hpp"

#include "maicord/sys/d3d11.hpp"
#include "maicord/render/glyph_atlas.hpp"

namespace maicord {
namespace {
struct Constants {
    u32 x, y, width, height;
    float left, top, scale;
    u32 first, padding, nodes;
    u32 program_base, reserved;
};
static_assert(sizeof(Constants) == 48);
bool initialize(ColrPaintGpu* g, ID3D11Device* device, u16 extent, bool surface) noexcept {
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    g->append_upload =
        SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
        options.MapNoOverwriteOnDynamicBufferSRV;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = extent;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_BUFFER_DESC c{};
    c.ByteWidth = sizeof(Constants);
    c.Usage = D3D11_USAGE_DYNAMIC;
    c.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    c.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if ((surface && !g->texture && (FAILED(device->CreateTexture2D(&d, nullptr, &g->texture)) ||
        FAILED(device->CreateShaderResourceView(g->texture, nullptr, &g->view)) ||
        FAILED(device->CreateUnorderedAccessView(g->texture, nullptr, &g->uav)))) ||
        (!g->constants && FAILED(device->CreateBuffer(&c, nullptr, &g->constants)))) {
        colr_paint_gpu_release(g);
        return false;
    }
    return true;
}
} // namespace

namespace {
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
void unbind_colr(ID3D11DeviceContext* context) noexcept {
    ID3D11UnorderedAccessView* uav=nullptr;
    ID3D11ShaderResourceView* views[2]{};
    ID3D11Buffer* buffer=nullptr;
    context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context->CSSetShaderResources(0,2,views);
    context->CSSetConstantBuffers(0,1,&buffer);
    context->CSSetShader(nullptr,nullptr,0);
}
ID3D11ComputeShader* ensure_colr(ColrPaintGpu* g,ID3D11DeviceContext* context,u32 bytes,bool general,u16 extent,bool external_target=false,bool layers=false) noexcept {
    auto** shader = layers ? &g->layers_shader : general ? &g->wide_shader : &g->shader;
    const bool initialize_resources = !g->constants || (!external_target && !g->texture);
    const bool resize = initialize_resources || bytes > g->capacity || !*shader;
    ID3D11Device* device = nullptr;
    if (resize)
        context->GetDevice(&device);
    bool ok = !resize || device;
    if (ok && initialize_resources)
        ok = initialize(g, device, extent, !external_target);
    if (ok && bytes > g->capacity) {
        // Retain upload slack without doubling a large paint's entire buffer.
        // Programs are bounded by the eight-MiB compilation arena.
        const u32 quantum = 256 * 1024;
        const u32 capacity = (bytes + quantum - 1) & ~(quantum - 1);
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = capacity;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        ID3D11Buffer* buffer = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        v.BufferEx.NumElements = capacity / 4;
        v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ok = SUCCEEDED(device->CreateBuffer(&d, nullptr, &buffer)) &&
             SUCCEEDED(device->CreateShaderResourceView(buffer, &v, &view));
        if (ok) {
            com_release(g->program_view);
            com_release(g->program);
            g->program = buffer;
            g->program_view = view;
            g->capacity = capacity;
            g->upload_cursor = 0;
        } else {
            com_release(view);
            com_release(buffer);
        }
    }
    if (ok && !*shader)
        ok = SUCCEEDED(shader_create_compute(device, layers ? ShaderId::colr_layers_compute :
            general ? ShaderId::colr_wide_compute : ShaderId::colr_compute, shader));
    com_release(device);
    return ok ? *shader : nullptr;
}
}
void ColrPaintBatch::reset() noexcept {
    if(active())unbind_colr(context);
    gpu=nullptr;program_view=nullptr;
}
static bool rasterize_colr(ColrPaintGpu* g, ID3D11DeviceContext* context, const ColrPaintProgram& p,
                          const GlyphRasterBounds& bounds, const GlyphAtlasAllocation& a, float scale,
                          u16 extent, u16 target_height, u16 padding, ColrPaintBatch* batch, ID3D11UnorderedAccessView* target) noexcept {
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
    auto* shader=ensure_colr(g,context,bytes,general,extent,target!=nullptr,layers);
    if(!shader)return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    // Append disjoint ranges until full. DISCARD renames the entire resource,
    // so doing it for every tiny glyph after a large one wastes driver memory.
    if (!g->append_upload || bytes > g->capacity - g->upload_cursor)
        g->upload_cursor = 0;
    const u32 program_base = g->upload_cursor;
    if (FAILED(context->Map(g->program, 0,
                            program_base ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0,
                            &mapped)))
        return false;
    auto* upload=static_cast<u8*>(mapped.pData)+program_base;
    CopyMemory(upload,p.storage.base,p.storage.offset);
    const u32 plan_bytes=plan_capacity?write_layer_plan(p,bounds,padding,upload):0;
    if(plan_capacity)++g->spatial_jobs;
    context->Unmap(g->program, 0);
    g->upload_cursor = (program_base + static_cast<u32>(p.storage.offset) + plan_bytes + 15) & ~15u;
    if (FAILED(context->Map(g->constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    auto& constants = *static_cast<Constants*>(mapped.pData);
    constants.x = static_cast<u32>(a.x - padding);
    constants.y = static_cast<u32>(a.y - padding);
    constants.width = static_cast<u32>(a.width + 2 * padding);
    constants.height = static_cast<u32>(a.height + 2 * padding);
    constants.left = static_cast<float>(bounds.left);
    constants.top = static_cast<float>(bounds.top);
    constants.scale = scale;
    constants.first = p.first;
    constants.padding = padding;
    constants.nodes = p.nodes;
    constants.program_base = program_base;
    constants.reserved = plan_capacity ? static_cast<u32>(p.storage.offset) : 0;
    context->Unmap(g->constants, 0);
    if(batch&&batch->gpu&&batch->gpu!=g)batch->reset();
    const bool retained=batch&&batch->active();
    if(!retained)context->CSSetConstantBuffers(0, 1, &g->constants);
    if(!retained||batch->program_view!=g->program_view)
        context->CSSetShaderResources(0, 1, &g->program_view);
    context->CSSetShader(shader, nullptr, 0);
    auto* output = target ? target : g->uav;
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((a.width + 2 * padding + 7) / 8, (a.height + 2 * padding + 7) / 8, 1);
    if(batch){batch->gpu=g;batch->program_view=g->program_view;}
    else unbind_colr(context);
    ++g->dispatches;
    return true;
}
bool colr_paint_rasterize(ColrPaintGpu* g, ID3D11DeviceContext* context, const ColrPaintProgram& p,
    const GlyphRasterBounds& bounds, const GlyphAtlasAllocation& allocation, float scale,
    u16 extent, u16 padding, ColrPaintBatch* batch) noexcept {
    return rasterize_colr(g,context,p,bounds,allocation,scale,extent,extent,padding,batch,nullptr);
}
bool colr_paint_rasterize_target(ColrPaintGpu* g, ID3D11DeviceContext* context,
    const ColrPaintProgram& p, const GlyphRasterBounds& bounds,
    const GlyphAtlasAllocation& allocation, float scale,
    u16 target_width, u16 target_height, u16 padding, ID3D11UnorderedAccessView* target) noexcept {
    if (!target || p.storage.offset > 8 * 1024 * 1024)
        return false;
    return rasterize_colr(g,context,p,bounds,allocation,scale,target_width,target_height,padding,nullptr,target);
}
void colr_paint_gpu_release(ColrPaintGpu* g) noexcept {
    if (!g)
        return;
    wchar_t metrics[2]{};
    if(g->dispatches&&GetEnvironmentVariableW(L"MAICORD_GPU_METRICS",metrics,2)) {
        log_u64(LogLevel::info,"render","colr_spatial_jobs",g->spatial_jobs);
        log_u64(LogLevel::info,"render","colr_total_jobs",g->dispatches);
    }
    com_release(g->wide_shader);
    com_release(g->layers_shader);
    com_release(g->shader);
    com_release(g->constants);
    com_release(g->program_view);
    com_release(g->program);
    com_release(g->uav);
    com_release(g->view);
    com_release(g->texture);
    reset_value(g);
}
} // namespace maicord
