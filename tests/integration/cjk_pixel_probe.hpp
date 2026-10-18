#pragma once
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
inline bool probe_cjk_pixel_stream() noexcept {
    wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_CJK_PIXEL_STREAM",value,2)==1&&value[0]=='1';
}
inline bool probe_cjk_pixels(D3D11Renderer& renderer,const wchar_t* options,
    bool (*capture)(const wchar_t*) noexcept) noexcept {
    if(!probe_cjk_pixel_stream()&&!GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR",nullptr,0))return false;
    u32 values[4]{};
    for(auto& value:values){while(*options==' ')++options;if(*options<'0'||*options>'9')return false;
        while(*options>='0'&&*options<='9'){if(value>1000000)return false;value=value*10+*options++-'0';}}
    if(*options||values[0]>1||values[1]!=text_style_heading_3||!values[2]||values[2]>1000000||values[3]>1)return false;
    wchar_t family[128]{},input[512]{};char encoded[2048]{};
    const auto family_size=GetEnvironmentVariableW(L"MAICORD_CJK_PIXEL_FAMILY",family,128);
    const auto input_size=GetEnvironmentVariableW(L"MAICORD_CJK_PIXEL_TEXT",input,512);
    if(!family_size||family_size>=128||!input_size||input_size>=512)return false;
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input,static_cast<int>(input_size),encoded,sizeof(encoded),nullptr,nullptr);
    if(bytes<=0)return false;const Utf8View text{encoded,static_cast<u32>(bytes)};
    auto& owner=renderer.glyph_text;auto* context=renderer.graphics.context;
    text_system_release(&owner.system);
    if(!text_system_initialize(&owner.system,family,14,L"en-US").ok())return false;
    const u32 first=values[0]?utf8_decode_one(text.data,text.bytes).width:0;
    const TextStyleSpan style{first,text.bytes-first,values[1]};
    if(!text_shape_utf8_styled(&owner.system,text,{&style,1},static_cast<float>(values[2]),1000000,
        {owner.utf16,kRendererGlyphCapacity},&owner.title,values[3]?TextWrapping::no_wrap:TextWrapping::wrap).ok())return false;
    GlyphDrawBatch batch{{0,0,1200,970},0,0};RendererTextStorage content{};
    content.quads=owner.title_quads;content.quad_capacity=kRendererGlyphCapacity;content.batches=&batch;content.batch_count=1;
    const u32 overlay=renderer.overlay_text_first_batch;renderer.content_text=&content;renderer.overlay_text_first_batch=1;
    struct Restore {D3D11Renderer& r;u32 overlay;~Restore() noexcept {
        r.content_text=nullptr;r.uploaded_text_storage=nullptr;r.overlay_text_first_batch=overlay;
    }} restore{renderer,overlay};
    const u32 dpi[]{96,120,144,192};const float origins[][2]{{0,0},{.25f,.5f},{.5f,.25f},{3.125f,-17.3f}};
    u32 frames=0;
    for(u32 scale=0;scale<4;++scale){renderer_set_dpi(&renderer,dpi[scale]);
        for(u32 origin=0;origin<4;++origin){owner.title_quad_count=0;bool invalidated=false;
            ColrPaintBatch colors(context);owner.color_batch=&colors;
            struct Reset {D3D11GlyphTextOwner& owner;~Reset() noexcept {owner.color_batch=nullptr;}} reset{owner};
            D3D11GlyphTextAppendRequest request{};request.text=&owner.title;request.output=owner.title_quads;
            request.count=&owner.title_quad_count;request.atlas_invalidated=&invalidated;request.source=text;
            request.color={1,1,1,1};request.origin_x=16+origins[origin][0];request.origin_y=32+origins[origin][1];
            request.capacity=kRendererGlyphCapacity;request.dpi=dpi[scale];
            if(!d3d11_glyph_text_append(&owner,context,request)||invalidated||!colors.finish())return false;
            colors.reset();content.quad_count=owner.title_quad_count;batch.quad_count=content.quad_count;++content.rebuilds;
            if(!content.quad_count)return false;
            for(u32 i=0;i<content.quad_count;++i){const auto& q=content.quads[i];const float factor=dpi[scale]/96.f;
                if(q.left<0||q.top<0||q.right*factor>1200||q.bottom*factor>970)return false;}
            const float clear[]{0,0,0,1};context->ClearRenderTargetView(renderer.graphics.target,clear);
            wchar_t filename[]=L"cjk-d0-o0.bmp";filename[5]=static_cast<wchar_t>('0'+scale);filename[8]=static_cast<wchar_t>('0'+origin);
            if(!capture(filename))return false;++frames;
        }
    }
    log_u64(LogLevel::info,"probe","cjk_pixel_frames",frames);return frames==16;
}
}
