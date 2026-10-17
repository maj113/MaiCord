// Glyph distance raster against a reference shader. Registered with MAICORD_GLYPH_DISTANCE_
// CONTROL 0-2. Manual: MAICORD_GLYPH_DISTANCE_VARIANT=<n> picks another random corpus and
// MAICORD_GLYPH_RASTER_RAW_REFERENCE=1 with MAICORD_GLYPH_RASTER_REFERENCE=<shader> compares
// an external reference.
#include "maicord/render/glyph_atlas_gpu.hpp"
#include "glyph_raster_reference.hpp"

namespace maicord { namespace {
constexpr u32 extent=64,frames=384,repeats=128;
void print(const char* p) noexcept {u32 n=0;while(p[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),p,n,&written,nullptr);}
void number(u64 value) noexcept {char digits[24];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(n){const char c=digits[--n];DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),&c,1,&written,nullptr);}}
#define CHECK(x) do{if(!(x)){print("FAIL line ");number(__LINE__);print("\n");return false;}}while(0)
struct State {
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;ID3D11Texture2D* staging=nullptr;
    bool raw_reference=false;
    ProbeGlyphRasterReference reference;
    GlyphAtlasGpu gpu[2]{};ID3D11Query* starts[2]{};ID3D11Query* ends[2]{};ID3D11Query* disjoint=nullptr;
    ~State() noexcept {if(context)context->ClearState();for(u32 i=0;i<2;++i){glyph_atlas_gpu_release(&gpu[i]);com_release(starts[i]);com_release(ends[i]);}
        com_release(disjoint);com_release(staging);com_release(context);com_release(device);}
};
bool query(State& s,ID3D11Query* q,void* p,u32 bytes,u64 deadline) noexcept {
    for(;;){const HRESULT hr=s.context->GetData(q,p,bytes,D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(hr!=S_FALSE)return hr==S_OK;if(GetTickCount64()>=deadline)return false;Sleep(1);}
}
void dispatch(State& s,u32 side,u32 count) noexcept {
    auto& gpu=s.gpu[side];
    if(!side&&!s.raw_reference)s.reference.bind(s.context,gpu.texture_uav);
    else{s.context->CSSetShader(gpu.raster_shader,nullptr,0);s.context->CSSetShaderResources(0,1,&gpu.command_view);
        s.context->CSSetUnorderedAccessViews(0,1,&gpu.texture_uav,nullptr);}
    for(u32 i=0;i<count;++i)s.context->Dispatch(8,8,1);
    ID3D11UnorderedAccessView* null_view=nullptr;s.context->CSSetUnorderedAccessViews(0,1,&null_view,nullptr);
}
bool rasterize(GlyphAtlasGpu* gpu,ID3D11DeviceContext* context,const GlyphOutline& outline,
    const GlyphRasterBounds& bounds,const GlyphAtlasAllocation& allocation,float scale,u16 padding) noexcept {
    GlyphRasterBatch batch(gpu,context);return batch.add(outline,bounds,allocation,scale,padding).ok()&&batch.finish();
}
bool run() noexcept {
    State s;CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&s.device,nullptr,&s.context)));
    for(auto& gpu:s.gpu)CHECK(glyph_atlas_gpu_initialize(&gpu,s.device,extent,extent,128).ok());
    wchar_t raw[2]{};s.raw_reference=GetEnvironmentVariableW(L"MAICORD_GLYPH_RASTER_RAW_REFERENCE",raw,2)==1&&raw[0]==L'1';
    if(s.raw_reference){
        CHECK(GetEnvironmentVariableW(L"MAICORD_GLYPH_RASTER_REFERENCE",nullptr,0)>1);
        com_release(s.gpu[0].raster_shader);
        CHECK(probe_create_glyph_raster_reference(s.device,&s.gpu[0].raster_shader));
    }else CHECK(s.reference.initialize(s.device,128));
    D3D11_TEXTURE2D_DESC td{};s.gpu[0].texture->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(s.device->CreateTexture2D(&td,nullptr,&s.staging)));
    wchar_t variant_text[2]{},control_text[2]{};
    GetEnvironmentVariableW(L"MAICORD_GLYPH_DISTANCE_VARIANT",variant_text,2);GetEnvironmentVariableW(L"MAICORD_GLYPH_DISTANCE_CONTROL",control_text,2);
    const u32 variant=variant_text[0]>=L'0'&&variant_text[0]<=L'9'?variant_text[0]-L'0':0;
    const u32 control=control_text[0]>=L'0'&&control_text[0]<=L'2'?control_text[0]-L'0':0;
    GlyphPathCommand commands[128]{};u32 last_count=0,random=0x83d245af^(variant*2654435761u);
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
        last_count=count;
        const float scales[]{1,4,8,12,16,24,32,48,64,96,144,1024,.125f,.5f,1.0e-12f,1.0e-20f};const float scale=scales[frame%16];
        const u16 padding=static_cast<u16>(frame%3);const u16 size=static_cast<u16>(extent-2*padding);
        GlyphOutline outline{commands,128,count,0,-1,-1,2,2,frame&1?GlyphFillRule::winding:GlyphFillRule::alternate,true};
        GlyphRasterBounds bounds{-static_cast<i32>(frame%5),-static_cast<i32>(frame%7),size,size,size,size};
        GlyphAtlasAllocation allocation{{0,1},padding,padding,size,size,0,GlyphAtlasError::none,false,false};
        for(u32 order=0;order<2;++order){const u32 side=order^(frame&1);const UINT clear[4]{};s.context->ClearUnorderedAccessViewUint(s.gpu[side].texture_uav,clear);
            CHECK(side||s.raw_reference?rasterize(&s.gpu[side],s.context,outline,bounds,allocation,scale,padding):
                s.reference.rasterize(s.context,s.gpu[0].texture_uav,outline,bounds,allocation,scale,padding));
            s.context->CopyResource(s.staging,s.gpu[side].texture);D3D11_MAPPED_SUBRESOURCE mapped{};
            CHECK(SUCCEEDED(s.context->Map(s.staging,0,D3D11_MAP_READ,0,&mapped)));
            for(u32 y=0;y<extent;++y)CopyMemory(pixels[side]+y*extent,static_cast<const u8*>(mapped.pData)+y*mapped.RowPitch,extent);
            s.context->Unmap(s.staging,0);
        }
        for(u32 i=0;i<extent*extent;++i){if(pixels[0][i]!=pixels[1][i]){print("Mismatch frame=");number(frame);print(" texel=");number(i);print(" old=");number(pixels[0][i]);print(" new=");number(pixels[1][i]);print("\n");return false;}
            ++compared;if(pixels[0][i])++painted;if(pixels[0][i]>0&&pixels[0][i]<255)++partial;}
    }
    CHECK(painted>10000&&partial>1000);
    for(u32 i=0;i<last_count;++i)if(commands[i].kind==GlyphPathCommandKind::line||commands[i].kind==GlyphPathCommandKind::cubic){
        if(control==1)commands[i].kind=GlyphPathCommandKind::line;else if(control==2)commands[i].kind=GlyphPathCommandKind::cubic;}
    GlyphOutline timed{commands,128,last_count,0,-1,-1,2,2,GlyphFillRule::winding,true};
    GlyphRasterBounds timed_bounds{0,0,62,62,62,62};GlyphAtlasAllocation timed_allocation{{0,1},1,1,62,62,0,GlyphAtlasError::none,false,false};
    CHECK(s.raw_reference?rasterize(&s.gpu[0],s.context,timed,timed_bounds,timed_allocation,24,1):
        s.reference.rasterize(s.context,s.gpu[0].texture_uav,timed,timed_bounds,timed_allocation,24,1));
    CHECK(rasterize(&s.gpu[1],s.context,timed,timed_bounds,timed_allocation,24,1));
    D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};CHECK(SUCCEEDED(s.device->CreateQuery(&q,&s.disjoint)));q.Query=D3D11_QUERY_TIMESTAMP;
    for(u32 i=0;i<2;++i)CHECK(SUCCEEDED(s.device->CreateQuery(&q,&s.starts[i]))&&SUCCEEDED(s.device->CreateQuery(&q,&s.ends[i])));
    print("{\"kind\":\"glyph_distance\",\"control\":");number(control);print(",\"variant\":");number(variant);print(",\"raw_reference\":");print(s.raw_reference?"true":"false");print(",\"frames\":");number(frames);print(",\"exact_texels\":");number(compared);print(",\"painted\":");number(painted);print(",\"partial\":");number(partial);print(",\"batches\":[");
    for(u32 batch=0;batch<6;++batch){dispatch(s,0,4);dispatch(s,1,4);s.context->Begin(s.disjoint);
        for(u32 order=0;order<2;++order){const u32 side=order^(batch&1);s.context->End(s.starts[side]);dispatch(s,side,repeats);s.context->End(s.ends[side]);}
        s.context->End(s.disjoint);s.context->Flush();const u64 deadline=GetTickCount64()+250;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        CHECK(query(s,s.disjoint,&frequency,sizeof(frequency),deadline)&&!frequency.Disjoint&&frequency.Frequency);
        if(batch)print(",");print("[");number(frequency.Frequency);
        for(u32 side=0;side<2;++side){u64 start=0,end=0;CHECK(query(s,s.starts[side],&start,sizeof(start),deadline)&&query(s,s.ends[side],&end,sizeof(end),deadline)&&end>=start);print(",");number(end-start);}print("]");}
    print("]}\nPASS glyph distance\n");return true;
}
}}
extern "C" void maicord_glyph_distance_entry(){ExitProcess(maicord::run()?0:1);}
