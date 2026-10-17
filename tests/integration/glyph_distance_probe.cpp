// Glyph distance raster against a reference shader: exact texels over a random corpus.
#include "maicord/render/glyph_atlas_gpu.hpp"
#include "glyph_raster_reference.hpp"
#include "../test_output.hpp"

namespace maicord { namespace {
constexpr u32 extent=64,frames=384;
#define CHECK(x) do{if(!(x)){test_write_text("FAIL line ");test_write_u64(__LINE__);test_write_text("\n");return false;}}while(0)
struct State {
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;ID3D11Texture2D* staging=nullptr;
    ProbeGlyphRasterReference reference;
    GlyphAtlasGpu gpu[2]{};
    ~State() noexcept {if(context)context->ClearState();for(u32 i=0;i<2;++i)glyph_atlas_gpu_release(&gpu[i]);
        com_release(staging);com_release(context);com_release(device);}
};
bool rasterize(GlyphAtlasGpu* gpu,ID3D11DeviceContext* context,const GlyphOutline& outline,
    const GlyphRasterBounds& bounds,const GlyphAtlasAllocation& allocation,float scale,u16 padding) noexcept {
    GlyphRasterBatch batch(gpu,context);return batch.add(outline,bounds,allocation,scale,padding).ok()&&batch.finish();
}
bool run() noexcept {
    State s;CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&s.device,nullptr,&s.context)));
    for(auto& gpu:s.gpu)CHECK(glyph_atlas_gpu_initialize(&gpu,s.device,extent,extent,128).ok());
    CHECK(s.reference.initialize(s.device,128));
    D3D11_TEXTURE2D_DESC td{};s.gpu[0].texture->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(s.device->CreateTexture2D(&td,nullptr,&s.staging)));
    GlyphPathCommand commands[128]{};u32 random=0x83d245af;
    const auto coordinate=[&]() noexcept {random=random*1664525u+1013904223u;return (static_cast<float>(random>>8)/16777216.0f)*3-1;};
    static u8 pixels[2][extent*extent];u64 compared=0,painted=0,partial=0;
    for(u32 frame=0;frame<frames;++frame){
        u32 count=0;
        for(u32 contour=0;contour<1+frame%4;++contour){
            commands[count++]={coordinate(),coordinate(),0,0,0,0,frame%13==0?GlyphPathCommandKind::begin_hollow:GlyphPathCommandKind::begin_filled,{}};
            for(u32 segment=0;segment<1+frame%11;++segment){
                const bool cubic=(frame+segment)%3!=0;
                commands[count++]={coordinate(),coordinate(),coordinate(),coordinate(),coordinate(),coordinate(),cubic?GlyphPathCommandKind::cubic:GlyphPathCommandKind::line,{}};
                if(segment==0&&frame%7==0){commands[count-1].x1=commands[count-2].x1;commands[count-1].y1=commands[count-2].y1;}
            }
            commands[count++]={0,0,0,0,0,0,GlyphPathCommandKind::end_closed,{}};
        }
        const float scales[]{1,4,8,12,16,24,32,48,64,96,144,1024,.125f,.5f,1.0e-12f,1.0e-20f};const float scale=scales[frame%16];
        const u16 padding=static_cast<u16>(frame%3);const u16 size=static_cast<u16>(extent-2*padding);
        GlyphOutline outline{commands,128,count,0,-1,-1,2,2,frame&1?GlyphFillRule::winding:GlyphFillRule::alternate,true};
        GlyphRasterBounds bounds{-static_cast<i32>(frame%5),-static_cast<i32>(frame%7),size,size,size,size};
        GlyphAtlasAllocation allocation{{0,1},padding,padding,size,size,0,GlyphAtlasError::none,false,false};
        for(u32 order=0;order<2;++order){const u32 side=order^(frame&1);const UINT clear[4]{};s.context->ClearUnorderedAccessViewUint(s.gpu[side].texture_uav,clear);
            CHECK(side?rasterize(&s.gpu[side],s.context,outline,bounds,allocation,scale,padding):
                s.reference.rasterize(s.context,s.gpu[0].texture_uav,outline,bounds,allocation,scale,padding));
            s.context->CopyResource(s.staging,s.gpu[side].texture);D3D11_MAPPED_SUBRESOURCE mapped{};
            CHECK(SUCCEEDED(s.context->Map(s.staging,0,D3D11_MAP_READ,0,&mapped)));
            for(u32 y=0;y<extent;++y)CopyMemory(pixels[side]+y*extent,static_cast<const u8*>(mapped.pData)+y*mapped.RowPitch,extent);
            s.context->Unmap(s.staging,0);
        }
        for(u32 i=0;i<extent*extent;++i){if(pixels[0][i]!=pixels[1][i]){test_write_text("Mismatch frame=");test_write_u64(frame);test_write_text(" texel=");test_write_u64(i);test_write_text(" old=");test_write_u64(pixels[0][i]);test_write_text(" new=");test_write_u64(pixels[1][i]);test_write_text("\n");return false;}
            ++compared;if(pixels[0][i])++painted;if(pixels[0][i]>0&&pixels[0][i]<255)++partial;}
    }
    CHECK(painted>10000&&partial>1000);
    test_write_text("{\"kind\":\"glyph_distance\",\"frames\":");test_write_u64(frames);test_write_text(",\"exact_texels\":");test_write_u64(compared);test_write_text(",\"painted\":");test_write_u64(painted);test_write_text(",\"partial\":");test_write_u64(partial);
    test_write_text("}\nPASS glyph distance\n");return true;
}
}}
extern "C" void maicord_glyph_distance_entry(){ExitProcess(maicord::run()?0:1);}
