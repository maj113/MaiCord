#include "maicord/render/d3d11_draw_pipeline.hpp"
#include "maicord/render/text_effect.hpp"

#include <d3d11_1.h>
#if defined(MAICORD_GLYPH_EASING_PROBE)
#if defined(MAICORD_GLYPH_COLOR_PROBE)
#include "maicord_glyph_vs.hpp"
#include "maicord_glyph_color_reference_ps.hpp"
#define g_maicord_glyph_easing_reference_vertex_shader g_maicord_glyph_vertex_shader
#define g_maicord_glyph_easing_reference_pixel_shader g_maicord_glyph_color_reference_pixel_shader
#elif defined(MAICORD_GLYPH_STAGE_PROBE)
#include "maicord_glyph_stage_reference_vs.hpp"
#include "maicord_glyph_stage_reference_ps.hpp"
#define g_maicord_glyph_easing_reference_vertex_shader g_maicord_glyph_stage_reference_vertex_shader
#define g_maicord_glyph_easing_reference_pixel_shader g_maicord_glyph_stage_reference_pixel_shader
#else
#include "maicord_glyph_easing_reference_vs.hpp"
#include "maicord_glyph_easing_reference_ps.hpp"
#endif
#define g_maicord_glyph_list_vertex_shader g_maicord_glyph_easing_reference_vertex_shader
#else
#include "maicord_glyph_list_vs.hpp"
#endif

namespace maicord { namespace {
constexpr u32 width=256,height=192,instances=128,repeats=256;
#if defined(MAICORD_GLYPH_EASING_PROBE)
constexpr u32 pixel_frames=178;
#else
constexpr u32 pixel_frames=24;
#endif
struct Quad {float left,top,right,bottom,u0,v0,u1,v1;Color color;};
static_assert(sizeof(Quad)==48);
struct State {
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;
    D3D11DrawPipelineOwner pipeline{};ID3D11VertexShader* reference=nullptr;
    ID3D11PixelShader* reference_pixel=nullptr;
    ID3D11Texture2D* target=nullptr;ID3D11Texture2D* staging=nullptr;
    ID3D11Texture2D* atlas[2]{};ID3D11ShaderResourceView* views[2]{};
    ID3D11RenderTargetView* rtv=nullptr;ID3D11Buffer* effects=nullptr;
    ID3D11Query* stats[2]{};ID3D11Query* starts[2]{};ID3D11Query* ends[2]{};ID3D11Query* disjoint=nullptr;
    ~State() noexcept {if(context)context->ClearState();
        for(u32 i=0;i<2;++i){com_release(stats[i]);com_release(starts[i]);com_release(ends[i]);com_release(views[i]);com_release(atlas[i]);}
        com_release(disjoint);com_release(effects);com_release(rtv);com_release(staging);com_release(target);com_release(reference);com_release(reference_pixel);
        d3d11_draw_pipeline_release(&pipeline);com_release(context);com_release(device);}
};
void print(const char* p) noexcept {u32 n=0;while(p[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),p,n,&written,nullptr);}
void number(u64 value) noexcept {char digits[24];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(n){const char c=digits[--n];DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),&c,1,&written,nullptr);}}
#define CHECK(x) do{if(!(x)){print("FAIL line ");number(__LINE__);print("\n");return false;}}while(0)
bool query(State& s,ID3D11Query* q,void* p,u32 bytes,u64 deadline) noexcept {
    for(;;){const HRESULT hr=s.context->GetData(q,p,bytes,D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(hr!=S_FALSE)return hr==S_OK;if(GetTickCount64()>=deadline)return false;Sleep(1);}
}
void draw(State& s,u32 side,u32 count) noexcept {
#if defined(MAICORD_GLYPH_EASING_PROBE)
    s.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    s.context->PSSetShader(side?s.pipeline.glyph_pixel_shader:s.reference_pixel,nullptr,0);
#else
    s.context->IASetPrimitiveTopology(side?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
#endif
    s.context->VSSetShader(side?s.pipeline.glyph_vertex_shader:s.reference,nullptr,0);
    for(u32 i=0;i<count;++i)s.context->DrawInstanced(
#if defined(MAICORD_GLYPH_EASING_PROBE)
        4,
#else
        side?4:6,
#endif
        instances,0,0);
}
bool run() noexcept {
#if defined(MAICORD_GLYPH_STAGE_PROBE)
    wchar_t control_text[2]{};const DWORD control_length=GetEnvironmentVariableW(L"MAICORD_GLYPH_STAGE_CONTROL",control_text,2);
    CHECK(!control_length||(control_length==1&&control_text[0]>=L'0'&&control_text[0]<=L'2'));
    const u32 control=control_length?static_cast<u32>(control_text[0]-L'0'):0;
#endif
    State s;CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&s.device,nullptr,&s.context)));
    CHECK(d3d11_draw_pipeline_initialize(&s.pipeline,s.device,s.context,width,height,96,{64,1,48,instances,48,1}).ok());
    CHECK(SUCCEEDED(s.device->CreateVertexShader(g_maicord_glyph_list_vertex_shader,sizeof(g_maicord_glyph_list_vertex_shader),nullptr,&s.reference)));
#if defined(MAICORD_GLYPH_EASING_PROBE)
    CHECK(SUCCEEDED(s.device->CreatePixelShader(g_maicord_glyph_easing_reference_pixel_shader,sizeof(g_maicord_glyph_easing_reference_pixel_shader),nullptr,&s.reference_pixel)));
#endif
    D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    CHECK(SUCCEEDED(s.device->CreateTexture2D(&td,nullptr,&s.target))&&SUCCEEDED(s.device->CreateRenderTargetView(s.target,nullptr,&s.rtv)));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(s.device->CreateTexture2D(&td,nullptr,&s.staging)));
    u8 mono[64]{};u32 rgba[64]{};for(u32 i=0;i<64;++i){mono[i]=static_cast<u8>((i*53)&255);rgba[i]=0x80402010u+(i%8)*0x00010203u;}
    td={};td.Width=td.Height=8;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    for(u32 i=0;i<2;++i){td.Format=i?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R8_UNORM;D3D11_SUBRESOURCE_DATA initial{i?static_cast<const void*>(rgba):static_cast<const void*>(mono),i?32u:8u,0};
        CHECK(SUCCEEDED(s.device->CreateTexture2D(&td,&initial,&s.atlas[i]))&&SUCCEEDED(s.device->CreateShaderResourceView(s.atlas[i],nullptr,&s.views[i])));}
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(TextEffectConstants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    CHECK(SUCCEEDED(s.device->CreateBuffer(&bd,nullptr,&s.effects)));
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT,0};CHECK(SUCCEEDED(s.device->CreateQuery(&qd,&s.disjoint)));
    for(u32 i=0;i<2;++i){qd.Query=D3D11_QUERY_PIPELINE_STATISTICS;CHECK(SUCCEEDED(s.device->CreateQuery(&qd,&s.stats[i])));
        qd.Query=D3D11_QUERY_TIMESTAMP;CHECK(SUCCEEDED(s.device->CreateQuery(&qd,&s.starts[i]))&&SUCCEEDED(s.device->CreateQuery(&qd,&s.ends[i])));}
    Quad quads[instances]{};for(u32 i=0;i<instances;++i){const float x=static_cast<float>(i%16)*15+.375f,y=static_cast<float>(i/16)*20+.125f;
        auto& v=quads[i];v={x,y,x+12.25f,y+16.5f,.0625f,.0625f,.9375f,.9375f,{.2f,.5f,.8f,.65f}};
        if(i%16<8)v.color={-static_cast<float>(i%8+1),static_cast<float>(i%7),x+6.125f,.75f};
        if(i%16==8){v.u0+=2;v.u1+=2;}
        if(i%16==9){v.u0=-3;v.u1=-2;v.v0=0;v.v1=1;}
        if(i%16==10)v.u0=v.u1=v.v0=v.v1=-1;
        if(i%16==11)v.color={-5,2,-1,.5f};
#if defined(MAICORD_GLYPH_STAGE_PROBE)
        // Cover a shadow for every effect, including overwritten toon/neon
        // intermediates, and effects on geometry that does not sample an atlas.
        if(i%16<8&&(i/16)&1){v.color.blue=-1;v.color.green=(i/16)&2?2.0f:.5f;}
        if(i/16>=4&&(i%16==9||i%16==10))v.color={-static_cast<float>(i/16-3),2,-1,.65f};
#endif
    }
#if defined(MAICORD_GLYPH_STAGE_PROBE)
    if(control)for(auto& v:quads){v.color={.2f,.5f,.8f,.65f};
        if(control==2){v.u0=2.0625f;v.u1=2.9375f;v.v0=.0625f;v.v1=.9375f;}}
#endif
    D3D11_MAPPED_SUBRESOURCE mapped{};CHECK(SUCCEEDED(s.context->Map(s.pipeline.glyph_instance_buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped)));
    CopyMemory(mapped.pData,quads,sizeof(quads));s.context->Unmap(s.pipeline.glyph_instance_buffer,0);
    const UINT stride=48,offset=0;const float blend[4]{};D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
    s.context->IASetInputLayout(s.pipeline.glyph_input_layout);s.context->IASetVertexBuffers(0,1,&s.pipeline.glyph_instance_buffer,&stride,&offset);
    s.context->VSSetConstantBuffers(0,1,&s.pipeline.ui_frame_buffer);s.context->VSSetConstantBuffers(1,1,&s.effects);
    s.context->PSSetConstantBuffers(1,1,&s.effects);s.context->PSSetShader(s.pipeline.glyph_pixel_shader,nullptr,0);
    s.context->PSSetShaderResources(0,2,s.views);s.context->PSSetSamplers(0,1,&s.pipeline.glyph_sampler);
    s.context->RSSetState(s.pipeline.ui_rasterizer_state);s.context->RSSetViewports(1,&viewport);s.context->OMSetBlendState(s.pipeline.ui_blend_state,blend,~0u);
    s.context->OMSetRenderTargets(1,&s.rtv,nullptr);
    static u8 pixels[2][width*height*4];u64 channels=0,painted=0;
    TextEffectConstants effects{};
    for(u32 frame=0;frame<pixel_frames;++frame){effects.seconds=static_cast<float>(frame)*.173f;effects.scale=1+static_cast<float>(frame%4)*.25f;
#if defined(MAICORD_GLYPH_EASING_PROBE)
        effects.seconds=static_cast<float>(frame)/16;
        if(frame>=64){const float boundaries[]{0,.07f,.14f,.15f,.16f,.18f,.20f,.21f,.22f,.23f,.25f,.28f,.35f,.5f,.51f,.75f,1,1.07f,1.51f};
            const u32 local=frame-64;effects.seconds=4*boundaries[local/6]+(static_cast<float>(local%3)-1)*.00002f;}
#endif
        for(u32 i=0;i<8;++i){auto& e=effects.materials[i];e.left=0;e.width=240;e.kind=static_cast<float>(i+1);e.count=6;
#if defined(MAICORD_GLYPH_COLOR_PROBE)
            e.count=static_cast<float>((frame+i)%8+1);
#endif
#if defined(MAICORD_GLYPH_EASING_PROBE)
            e.animate=frame<64||((frame-64)%6)<3?1.0f:0.0f;
#else
            e.animate=frame<16?1.0f:0.0f;
#endif
            e.reserved[0]=0;e.reserved[1]=18;e.reserved[2]=40;
            for(u32 k=0;k<8;++k){
#if defined(MAICORD_GLYPH_COLOR_PROBE)
                const u32 packed=(frame*1664525u+i*1013904223u+k*22695477u)^0x6d2b79f5u;
                e.colors[k]={static_cast<float>((packed>>16)&255)/255,static_cast<float>((packed>>8)&255)/255,static_cast<float>(packed&255)/255,1};
#else
                e.colors[k]={static_cast<float>((k*3)%7)/7,static_cast<float>((k*5)%7)/7,static_cast<float>(k)/8,1};
#endif
            }}
        s.context->UpdateSubresource(s.effects,0,nullptr,&effects,0,0);
        CHECK(d3d11_draw_pipeline_update_transform(s.pipeline,s.context,width,height,96+(frame%4)*24));
        const D3D11_RECT clip{static_cast<LONG>(frame%3)*7,3,static_cast<LONG>(width)-5,static_cast<LONG>(height)-3};s.context->RSSetScissorRects(1,&clip);
        for(u32 order=0;order<2;++order){const u32 side=order^(frame&1);const float clear[4]{};s.context->ClearRenderTargetView(s.rtv,clear);draw(s,side,1);
            s.context->CopyResource(s.staging,s.target);CHECK(SUCCEEDED(s.context->Map(s.staging,0,D3D11_MAP_READ,0,&mapped)));
            for(u32 y=0;y<height;++y)CopyMemory(pixels[side]+y*width*4,static_cast<const u8*>(mapped.pData)+y*mapped.RowPitch,width*4);
            s.context->Unmap(s.staging,0);}
        for(u32 i=0;i<width*height*4;++i){CHECK(pixels[0][i]==pixels[1][i]);++channels;if(pixels[0][i])++painted;}
    }
    CHECK(painted>100000);
#if defined(MAICORD_GLYPH_STAGE_PROBE)
#if defined(MAICORD_GLYPH_COLOR_PROBE)
    print("{\"kind\":\"glyph_color\",\"control\":");number(control);print(",\"pixel_frames\":");
#else
    print("{\"kind\":\"glyph_stage\",\"control\":");number(control);print(",\"pixel_frames\":");
#endif
#elif defined(MAICORD_GLYPH_EASING_PROBE)
    print("{\"kind\":\"glyph_easing\",\"pixel_frames\":");
#else
    print("{\"kind\":\"glyph_topology\",\"pixel_frames\":");
#endif
    number(pixel_frames);print(",\"exact_channels\":");number(channels);print(",\"nonzero_channels\":");number(painted);print(",\"batches\":[");
    for(u32 batch=0;batch<6;++batch){
#if defined(MAICORD_GLYPH_EASING_PROBE)
        effects.seconds=.12f+static_cast<float>(batch)*.22f;
        for(auto& e:effects.materials)e.animate=1;
        s.context->UpdateSubresource(s.effects,0,nullptr,&effects,0,0);
#endif
        draw(s,0,16);draw(s,1,16);s.context->Begin(s.disjoint);
        for(u32 order=0;order<2;++order){const u32 side=order^(batch&1);s.context->Begin(s.stats[side]);s.context->End(s.starts[side]);draw(s,side,repeats);s.context->End(s.ends[side]);s.context->End(s.stats[side]);}
        s.context->End(s.disjoint);s.context->Flush();const u64 deadline=GetTickCount64()+250;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        CHECK(query(s,s.disjoint,&frequency,sizeof(frequency),deadline)&&!frequency.Disjoint&&frequency.Frequency);
        if(batch)print(",");print("[");number(frequency.Frequency);
        u64 reference_vertices=0,reference_pixels=0;
        for(u32 side=0;side<2;++side){u64 begin=0,end=0;D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
            CHECK(query(s,s.starts[side],&begin,sizeof(begin),deadline)&&query(s,s.ends[side],&end,sizeof(end),deadline)&&query(s,s.stats[side],&stats,sizeof(stats),deadline)&&end>=begin);
#if defined(MAICORD_GLYPH_EASING_PROBE)
            CHECK(stats.IAVertices==4u*instances*repeats&&stats.IAPrimitives==2u*instances*repeats);
            if(side)CHECK(stats.VSInvocations==reference_vertices&&stats.PSInvocations==reference_pixels);
#else
            CHECK(stats.IAVertices==(side?4u:6u)*instances*repeats&&stats.IAPrimitives==2u*instances*repeats);
            if(side)CHECK(stats.VSInvocations<reference_vertices&&stats.PSInvocations==reference_pixels);
#endif
            else {reference_vertices=stats.VSInvocations;reference_pixels=stats.PSInvocations;}
            print(",");number(end-begin);print(",");number(stats.IAVertices);print(",");number(stats.VSInvocations);print(",");number(stats.IAPrimitives);print(",");number(stats.PSInvocations);}
        print("]");}
    print("]}\nPASS glyph topology\n");return true;
}
}}
extern "C" void maicord_glyph_topology_entry(){ExitProcess(maicord::run()?0:1);}
