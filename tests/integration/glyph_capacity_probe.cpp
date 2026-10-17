// Glyph command buffer growth. The registered run is the exact-texel comparison.
// MAICORD_GLYPH_GROWTH_FAILURE=1 is manual: it asserts a debugger-injected E_OUTOFMEMORY on
// buffer creation and fails without the injection.
#include "maicord/render/glyph_atlas_gpu.hpp"

#include <d3d11_1.h>
namespace maicord { namespace {
void print(const char* p) noexcept {u32 n=0;while(p[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),p,n,&written,nullptr);}
void number(u64 value) noexcept {char digits[24];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(n){const char c=digits[--n];DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),&c,1,&written,nullptr);}}
#define CHECK(x) do{if(!(x)){print("FAIL line ");number(__LINE__);print("\n");return false;}}while(0)
struct State {
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;ID3D11Texture2D* staging=nullptr;
    GlyphAtlasGpu gpu{},control{};
    ~State() noexcept {if(context)context->ClearState();glyph_atlas_gpu_release(&gpu);glyph_atlas_gpu_release(&control);com_release(staging);com_release(context);com_release(device);}
};
bool descriptor(const GlyphAtlasGpu& gpu) noexcept {
    D3D11_BUFFER_DESC b{};D3D11_SHADER_RESOURCE_VIEW_DESC v{};gpu.command_buffer->GetDesc(&b);gpu.command_view->GetDesc(&v);
    return b.ByteWidth==glyph_raster_header_bytes+gpu.command_committed*sizeof(GlyphPathCommand)&&!b.StructureByteStride&&
        b.MiscFlags==D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS&&v.Format==DXGI_FORMAT_R32_TYPELESS&&
        v.ViewDimension==D3D11_SRV_DIMENSION_BUFFEREX&&v.BufferEx.Flags==D3D11_BUFFEREX_SRV_FLAG_RAW&&
        b.Usage==D3D11_USAGE_DYNAMIC&&v.BufferEx.NumElements==b.ByteWidth/4;
}
// One raster job submitted as its own dispatch.
GlyphAtlasGpuResult rasterize(GlyphAtlasGpu* gpu,ID3D11DeviceContext* context,const GlyphOutline& outline,
    const GlyphRasterBounds& bounds,const GlyphAtlasAllocation& allocation,float scale,u16 padding) noexcept {
    GlyphRasterBatch batch(gpu,context);auto result=batch.add(outline,bounds,allocation,scale,padding);
    if(result.ok()&&!batch.finish())result=GlyphAtlasGpuResult{E_FAIL,GlyphAtlasGpuError::upload};
    return result;
}
bool run() noexcept {
    State s;CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&s.device,nullptr,&s.context)));
    CHECK(glyph_atlas_gpu_initialize(&s.gpu,s.device,32,32,2048).ok());
    CHECK(s.gpu.command_capacity==2048&&s.gpu.command_committed==128&&descriptor(s.gpu));
    D3D11_TEXTURE2D_DESC td{};s.gpu.texture->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(s.device->CreateTexture2D(&td,nullptr,&s.staging)));
    static GlyphPathCommand commands[65536];
    const auto shape=[&](u32 count) noexcept {
        for(u32 i=0;i<count;++i)commands[i]={0,1,0,0,0,0,GlyphPathCommandKind::line,{}};
        commands[0]={0,0,0,0,0,0,GlyphPathCommandKind::begin_filled,{}};
        commands[1]={1,0,0,0,0,0,GlyphPathCommandKind::line,{}};
        commands[2]={1,1,0,0,0,0,GlyphPathCommandKind::line,{}};
        commands[count-1]={0,0,0,0,0,0,GlyphPathCommandKind::end_closed,{}};
        return GlyphOutline{commands,65536,count,0,0,0,1,1,GlyphFillRule::winding,true};
    };
    GlyphRasterBounds bounds{0,0,8,8,8,8};GlyphAtlasAllocation allocation{{0,1},1,1,8,8,0,GlyphAtlasError::none,false,false};
    wchar_t failure[2]{};const bool inject=GetEnvironmentVariableW(L"MAICORD_GLYPH_GROWTH_FAILURE",failure,2)==1;
    if(inject){auto* old_buffer=s.gpu.command_buffer;auto* old_view=s.gpu.command_view;
        const auto failed=rasterize(&s.gpu,s.context,shape(129),bounds,allocation,8,1);
        CHECK(failed.error==GlyphAtlasGpuError::create_resource&&s.gpu.command_buffer==old_buffer&&s.gpu.command_view==old_view&&s.gpu.command_committed==128&&descriptor(s.gpu));
        CHECK(rasterize(&s.gpu,s.context,shape(5),bounds,allocation,8,1).ok());
        CHECK(rasterize(&s.gpu,s.context,shape(129),bounds,allocation,8,1).ok());
        CHECK(s.gpu.command_committed==256&&descriptor(s.gpu));print("PASS growth failure preserves buffer and retry\n");return true;}
    CHECK(glyph_atlas_gpu_initialize(&s.control,s.device,32,32,2048).ok());
    CHECK(rasterize(&s.control,s.context,shape(2048),bounds,allocation,8,1).ok());
    CHECK(s.control.command_committed==2048&&descriptor(s.control));
    const u32 counts[]{5,128,129,256,257,512,513,1024,1025,2048,5};
    u8 pixels[2][1024]{};u32 compared=0;print("{\"kind\":\"glyph_command_growth\",\"owner_bytes\":");number(sizeof(GlyphAtlasGpu));print(",\"capacities\":[");
    for(u32 test=0;test<11;++test){const auto outline=shape(counts[test]);const UINT clear[4]{};
        auto* old_buffer=s.gpu.command_buffer;const u32 old_capacity=s.gpu.command_committed;
        for(u32 side=0;side<2;++side){auto& gpu=side?s.control:s.gpu;s.context->ClearUnorderedAccessViewUint(gpu.texture_uav,clear);
            CHECK(rasterize(&gpu,s.context,outline,bounds,allocation,8,1).ok()&&descriptor(gpu));
            s.context->CopyResource(s.staging,gpu.texture);D3D11_MAPPED_SUBRESOURCE mapped{};
            CHECK(SUCCEEDED(s.context->Map(s.staging,0,D3D11_MAP_READ,0,&mapped)));
            for(u32 y=0;y<32;++y)CopyMemory(pixels[side]+y*32,static_cast<const u8*>(mapped.pData)+y*mapped.RowPitch,32);
            s.context->Unmap(s.staging,0);}
        if(counts[test]<=old_capacity)CHECK(s.gpu.command_buffer==old_buffer&&s.gpu.command_committed==old_capacity);
        else CHECK(s.gpu.command_buffer!=old_buffer&&s.gpu.command_committed>=counts[test]);
        for(u32 i=0;i<1024;++i){CHECK(pixels[0][i]==pixels[1][i]);++compared;}
        CHECK(pixels[0][4*32+4]==255&&pixels[0][0]==0);
        if(test)print(",");number(s.gpu.command_committed);
    }
    auto* full=s.gpu.command_buffer;const u32 dispatches=s.gpu.dispatch_count;
    CHECK(rasterize(&s.gpu,s.context,shape(2049),bounds,allocation,8,1).error==GlyphAtlasGpuError::command_capacity);
    CHECK(s.gpu.command_buffer==full&&s.gpu.command_committed==2048&&s.gpu.dispatch_count==dispatches);
    // Grow while the previous buffer's dispatch is still queued. Both tiles
    // must survive; no CPU readback separates the two submissions.
    glyph_atlas_gpu_release(&s.gpu);CHECK(glyph_atlas_gpu_initialize(&s.gpu,s.device,32,32,2048).ok());
    for(u32 side=0;side<2;++side){auto& gpu=side?s.control:s.gpu;const UINT clear[4]{};s.context->ClearUnorderedAccessViewUint(gpu.texture_uav,clear);
        allocation.x=1;CHECK(rasterize(&gpu,s.context,shape(5),bounds,allocation,8,1).ok());
        allocation.x=17;CHECK(rasterize(&gpu,s.context,shape(129),bounds,allocation,8,1).ok());
        s.context->CopyResource(s.staging,gpu.texture);D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(SUCCEEDED(s.context->Map(s.staging,0,D3D11_MAP_READ,0,&mapped)));
        for(u32 y=0;y<32;++y)CopyMemory(pixels[side]+y*32,static_cast<const u8*>(mapped.pData)+y*mapped.RowPitch,32);
        s.context->Unmap(s.staging,0);}
    CHECK(s.gpu.command_committed==256&&descriptor(s.gpu)&&pixels[0][4*32+4]==255&&pixels[0][4*32+20]==255);
    for(u32 i=0;i<1024;++i){CHECK(pixels[0][i]==pixels[1][i]);++compared;}
    glyph_atlas_gpu_release(&s.gpu);CHECK(!s.gpu.command_buffer&&!s.gpu.command_view&&!s.gpu.command_capacity&&!s.gpu.command_committed);
    CHECK(glyph_atlas_gpu_initialize(&s.gpu,s.device,32,32,5).ok()&&s.gpu.command_committed==5&&descriptor(s.gpu));
    CHECK(rasterize(&s.gpu,s.context,shape(5),bounds,allocation,8,1).ok());
    CHECK(glyph_atlas_gpu_initialize(&s.gpu,s.device,32,32,0).error==GlyphAtlasGpuError::invalid_input&&s.gpu.command_capacity==5);
    CHECK(glyph_atlas_gpu_initialize(&s.gpu,s.device,32,32,65536).error==GlyphAtlasGpuError::invalid_input&&s.gpu.command_capacity==5);
    glyph_atlas_gpu_release(&s.gpu);CHECK(glyph_atlas_gpu_initialize(&s.gpu,s.device,32,32,65535).ok()&&s.gpu.command_capacity==65535&&s.gpu.command_committed==128);
    bounds={0,0,6,6,6,6};allocation.width=allocation.height=6;
    for(u32 mode=0;mode<3;++mode){
        for(u32 side=0;side<2;++side){auto& gpu=side?s.control:s.gpu;const UINT clear[4]{};
            s.context->ClearUnorderedAccessViewUint(gpu.texture_uav,clear);
            const u32 raster_first=gpu.raster_count,dispatch_first=gpu.dispatch_count;
            GlyphRasterBatch batch(&gpu,s.context);
            for(u32 job=0;job<64;++job){
                const auto outline=shape((mode==1?40:5)+job%4);
                commands[1].x1=commands[2].x1=0.3f+0.1f*(job%7);
                allocation.x=static_cast<u16>(1+(mode==2?0:(job%4)*8));
                allocation.y=static_cast<u16>(1+(mode==2?0:((job/4)%4)*8));
                CHECK((side?rasterize(&gpu,s.context,outline,bounds,allocation,6,1):
                    batch.add(outline,bounds,allocation,6,1)).ok());
            }
            CHECK(batch.finish()&&!batch.pending()&&gpu.raster_count-raster_first==64);
            CHECK(side||mode==2?gpu.dispatch_count-dispatch_first==64:gpu.dispatch_count-dispatch_first<64);
            s.context->CopyResource(s.staging,gpu.texture);D3D11_MAPPED_SUBRESOURCE mapped{};
            CHECK(SUCCEEDED(s.context->Map(s.staging,0,D3D11_MAP_READ,0,&mapped)));
            for(u32 y=0;y<32;++y)CopyMemory(pixels[side]+y*32,static_cast<const u8*>(mapped.pData)+y*mapped.RowPitch,32);
            s.context->Unmap(s.staging,0);
        }
        for(u32 i=0;i<1024;++i){CHECK(pixels[0][i]==pixels[1][i]);++compared;}
    }
    {GlyphRasterBatch batch(&s.gpu,s.context);const u32 rasters=s.gpu.raster_count;
        CHECK(batch.add(shape(5),bounds,allocation,6,1).ok()&&batch.pending());
        batch.cancel();CHECK(!batch.pending()&&batch.finish()&&s.gpu.raster_count==rasters);
        CHECK(batch.add(shape(7),bounds,allocation,6,1).ok()&&batch.finish()&&s.gpu.raster_count==rasters+1);
    }
    glyph_atlas_gpu_release(&s.control);CHECK(glyph_atlas_gpu_initialize(&s.control,s.device,32,32,65535).ok());
    bounds={0,0,1,1,1,1};allocation.x=allocation.y=1;allocation.width=allocation.height=1;
    for(u32 side=0;side<2;++side){auto& gpu=side?s.control:s.gpu;
        CHECK(rasterize(&gpu,s.context,shape(65535),bounds,allocation,1,0).ok());
        const UINT clear[4]{};s.context->ClearUnorderedAccessViewUint(gpu.texture_uav,clear);
        const u32 before=gpu.dispatch_count;GlyphRasterBatch batch(&gpu,s.context);
        for(u32 job=0;job<2;++job){allocation.x=static_cast<u16>(1+job*16);const auto outline=shape(job?5:65530);
            CHECK((side?rasterize(&gpu,s.context,outline,bounds,allocation,1,0):batch.add(outline,bounds,allocation,1,0)).ok());
        }
        CHECK(batch.finish()&&gpu.dispatch_count-before==(side?2u:1u));
        s.context->CopyResource(s.staging,gpu.texture);D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(SUCCEEDED(s.context->Map(s.staging,0,D3D11_MAP_READ,0,&mapped)));
        for(u32 y=0;y<32;++y)CopyMemory(pixels[side]+y*32,static_cast<const u8*>(mapped.pData)+y*mapped.RowPitch,32);
        s.context->Unmap(s.staging,0);
    }
    CHECK(pixels[0][33]==255&&pixels[0][49]==255);
    for(u32 i=0;i<1024;++i){CHECK(pixels[0][i]==pixels[1][i]);++compared;}
    print("],\"exact_texels\":");number(compared);print("}\nPASS glyph command growth\n");return true;
}
}}
extern "C" void maicord_glyph_capacity_entry(){ExitProcess(maicord::run()?0:1);}
