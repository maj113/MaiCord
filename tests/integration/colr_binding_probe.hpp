#pragma once
#include "maicord/render/colr_paint.hpp"
#include "maicord/core/log.hpp"

#include "maicord/sys/d3d11.hpp"
namespace maicord {
inline bool colr_compute_bindings_empty(ID3D11DeviceContext* context) noexcept {
    ID3D11Buffer* constant=nullptr;ID3D11ComputeShader* shader=nullptr;
    ID3D11ShaderResourceView* views[2]{};ID3D11UnorderedAccessView* outputs[2]{};
    context->CSGetConstantBuffers(0,1,&constant);context->CSGetShader(&shader,nullptr,nullptr);
    context->CSGetShaderResources(0,2,views);context->CSGetUnorderedAccessViews(0,2,outputs);
    const bool empty=!constant&&!shader&&!views[0]&&!views[1]&&!outputs[0]&&!outputs[1];
    com_release(constant);com_release(shader);
    for(u32 i=0;i<2;++i){com_release(views[i]);com_release(outputs[i]);}
    return empty;
}
// Hand-built postfix programs over one square mask, one per kernel the
// rasterizer selects: a lone shaped source is a layered program (layers
// kernel), a shaped source composited over a pushed value is general with a
// shallow stack (narrow kernel), and nine pushed values need the 64-entry
// stack (wide kernel). Every program paints the same exact square.
inline bool colr_binding_probe(ID3D11Device* device,ID3D11DeviceContext* context) noexcept {
    ColrPaintGpu gpu{},other{};ColrPaintProgram program{};ID3D11Texture2D* staging=nullptr;
    struct Release {ColrPaintGpu& g;ColrPaintGpu& other;ColrPaintProgram& p;ID3D11Texture2D*& s;
        ~Release() noexcept {com_release(s);colr_paint_gpu_release(&g);colr_paint_gpu_release(&other);arena_release(&p.storage);}} release{gpu,other,program,staging};
    u32 stage=0;
    const auto fail=[&stage](u32 failed) noexcept {
        log_u64(LogLevel::error,"probe","colr_binding_failure_stage",failed);stage=failed;return false;};
    if(!colr_compute_bindings_empty(context))return fail(1);
    if(!arena_initialize(&program.storage,512*1024,512*1024,4096))return fail(2);
    u8* base=program.storage.base;
    constexpr u32 kSize=sizeof(ColrPaintNode);
    constexpr u32 kShallow=768,kShallowComposite=kShallow+kSize,kDeep=kShallowComposite+kSize;
    constexpr u32 kDeepPushes=8,kProgramEnd=kDeep+kSize*(1+kDeepPushes*2);
    const auto node_at=[base](u32 offset) noexcept {return reinterpret_cast<ColrPaintNode*>(base+offset);};
    // A shaped solid (kind 8) reads the mask at offset 72; its color is set per tile.
    const auto shaped=[&](u32 offset,u32 next) noexcept {
        auto* n=node_at(offset);n->next=next;n->kind=8;n->reserved=72;n->inverse[0]=n->inverse[3]=1;n->values[3]=1;};
    const auto push=[&](u32 offset,u32 next) noexcept {auto* n=node_at(offset);n->next=next;n->kind=0;};
    // Source-over with the transparent upper value keeps the lower value.
    const auto over=[&](u32 offset,u32 next) noexcept {auto* n=node_at(offset);n->next=next;n->kind=6;n->mode=3;n->count=1;};
    shaped(0,kShallow);push(kShallow,kShallowComposite);over(kShallowComposite,0);
    // Reverse order keeps the lower green source over an opaque red value.
    node_at(kShallowComposite)->count=0;
    node_at(kShallow)->values[0]=node_at(kShallow)->values[3]=1;
    shaped(kDeep,kDeep+kSize);
    for(u32 i=0;i<kDeepPushes*2;++i){
        const u32 at=kDeep+kSize*(1+i),next=i+1<kDeepPushes*2?at+kSize:0;
        if(i<kDeepPushes)push(at,next);else over(at,next);
    }
    // Forward order keeps an opaque yellow value over the deep stack's red base.
    node_at(kDeep+kSize)->values[0]=node_at(kDeep+kSize)->values[1]=node_at(kDeep+kSize)->values[3]=1;
    // Independent square mask. Extra empty rows exercise larger mask bounds.
    auto* job=reinterpret_cast<ColrMask*>(base+72);
    job->segments=160;job->segment_count=4;job->fill=1;job->width=job->height=2;
    job->bounds[2]=job->bounds[3]=.125f;job->rows=224;
    const float edges[]{0,0,.125f,0,.125f,0,.125f,.125f,.125f,.125f,0,.125f,0,.125f,0,0};
    copy_bytes(base+160,edges,sizeof(edges));
    auto* rows=reinterpret_cast<u32*>(base+224);
    rows[0]=736;rows[1]=4;rows[2]=752;rows[3]=4;
    auto* indices=reinterpret_cast<u32*>(base+736);
    for(u32 i=0;i<8;++i)indices[i]=i&3;
    program.storage.offset=kProgramEnd;program.stack=1;
    program.mask_count=1;program.mask_pixels=4;
    struct Shape {u32 first,nodes,peak_stack;};
    const Shape layered{0,1,1},shallow{0,3,2},deep{kDeep,1+kDeepPushes*2,1+kDeepPushes};
    const Shape shapes[4]{layered,shallow,layered,deep};
    const u8 colors[4][4]{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255}};
    const GlyphRasterBounds rectangle{0,0,2,2,2,2};
    {
        ColrPaintBatch batch(context);
        for(u32 i=0;i<4;++i){
            // The third tile grows the upload buffer past its first quantum.
            if(i==2){program.storage.offset=300*1024;job->width=job->height=64;
                program.mask_pixels=4096;}
            program.first=shapes[i].first;program.nodes=shapes[i].nodes;program.peak_stack=shapes[i].peak_stack;
            for(u32 c=0;c<3;++c)node_at(0)->values[c]=node_at(kDeep)->values[c]=colors[i][c]/255.f;
            if(i==3)node_at(kDeep)->values[1]=0;
            GlyphAtlasAllocation allocation{};allocation.x=static_cast<u16>(1+i*4);allocation.y=1;allocation.width=allocation.height=2;
            if(!colr_paint_rasterize(&gpu,context,program,rectangle,allocation,16,32,1,&batch)||
               !batch.active())return fail(10+i);
        }
        // Two layered jobs ran the layers kernel; the deep program created the wide one.
        if(gpu.capacity<300*1024||!gpu.shader||!gpu.layers_shader||!gpu.wide_shader||gpu.spatial_jobs!=2||
           gpu.dispatches!=4){
            log_u64(LogLevel::error,"probe","colr_binding_capacity",gpu.capacity);
            log_u64(LogLevel::error,"probe","colr_binding_spatial_jobs",gpu.spatial_jobs);
            return fail(4);
        }
        // Owner changes must bind that owner's constants and program even
        // while the batch still holds the previous owner's bindings.
        for(u32 i=0;i<2;++i){
            auto& selected=i?gpu:other;
            GlyphAtlasAllocation allocation{};allocation.x=i?13:1;allocation.y=1;allocation.width=allocation.height=2;
            if(!colr_paint_rasterize(&selected,context,program,rectangle,allocation,16,32,1,&batch))return fail(20+i);
            ID3D11Buffer* bound=nullptr;context->CSGetConstantBuffers(0,1,&bound);
            const bool correct=bound==selected.constants;com_release(bound);if(!correct)return fail(23+i*2);
            ID3D11ShaderResourceView* bound_program=nullptr;context->CSGetShaderResources(0,1,&bound_program);
            const bool correct_program=bound_program==selected.program_view;com_release(bound_program);if(!correct_program)return fail(24+i*2);
        }
        // Failed submission must still leave a scope that releases references.
        GlyphAtlasAllocation invalid{};
        if(colr_paint_rasterize(&gpu,context,program,rectangle,invalid,16,32,1,&batch))return fail(5);
    }
    if(!colr_compute_bindings_empty(context))return fail(6);
    D3D11_TEXTURE2D_DESC description{};gpu.texture->GetDesc(&description);
    description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(device->CreateTexture2D(&description,nullptr,&staging)))return fail(7);
    context->CopyResource(staging,gpu.texture);D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped)))return fail(8);
    bool exact=true;
    for(u32 tile=0;tile<4;++tile)for(u32 y=0;y<4;++y)for(u32 x=0;x<4;++x)for(u32 c=0;c<4;++c){
        const auto value=static_cast<const u8*>(mapped.pData)[y*mapped.RowPitch+(tile*4+x)*4+c];
        exact=exact&&value==((x==1||x==2)&&(y==1||y==2)?colors[tile][c]:0);
    }
    context->Unmap(staging,0);
    log_write(exact?LogLevel::info:LogLevel::error,"probe",exact?"colr_binding_kernels_growth_failure_and_256_channels_passed":"colr_binding_pixels_failed");
    return exact&&!stage;
}
}
