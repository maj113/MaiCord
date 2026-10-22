#pragma once
#include "emoji_gpu_measurement.hpp"
#include "maicord/render/colr_paint.hpp"
#include "maicord/core/hash.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
namespace maicord {
inline bool probe_glyph_cache(D3D11Renderer& renderer,bool (*capture)(const wchar_t*) noexcept) noexcept {
    const auto fail=[](u32 line) noexcept {log_u64(LogLevel::error,"glyph_cache","failure_line",line);return false;};
    auto& owner=renderer.glyph_text;auto* context=renderer.graphics.context;
    EmojiGpuMeasurement measurement;
    if(!measurement.initialize(renderer,5,L"MAICORD_GLYPH_CACHE_METRICS")||!measurement.enabled||
       owner.cache_builds||owner.title_quad_count||measurement.baseline.builds)return fail(__LINE__);
    D3D11GlyphGpuFootprint empty{};
    if(!d3d11_glyph_text_gpu_footprint(owner,&empty)||empty.mono_dispatches||empty.color_dispatches)return fail(__LINE__);
    if(renderer.content_text)return fail(__LINE__);
    GlyphDrawBatch content_batch{{0,0,800,600},0,0};
    RendererTextStorage content{};content.quads=owner.title_quads;content.quad_capacity=kRendererGlyphCapacity;content.batches=&content_batch;content.batch_count=1;
    const u32 previous_overlay=renderer.overlay_text_first_batch;
    renderer.content_text=&content;renderer.overlay_text_first_batch=1;
    struct RestoreContent {D3D11Renderer& renderer;u32 overlay;~RestoreContent() noexcept {
        renderer.content_text=nullptr;renderer.uploaded_text_storage=nullptr;renderer.overlay_text_first_batch=overlay;
    }} restore_content{renderer,previous_overlay};
    constexpr Utf8View texts[]{
        utf8_literal("AV office café 日本語 カタカナ 한국어\nאבג العربية नमस्ते ภาษาไทย\n😀 👩🏽‍💻 ❤️ 🇸🇮"),
        utf8_literal("AV office café 日本語 カタカナ 한국어\nאבג العربية नमस्ते ภาษาไทย"),
        utf8_literal("😀 👩🏽‍💻 ❤️ 🇸🇮")};
    wchar_t choice[2]{};const u32 selected=GetEnvironmentVariableW(L"MAICORD_GLYPH_CACHE_KIND",choice,2);
    if(selected&&(selected!=1||choice[0]<L'0'||choice[0]>L'2'))return fail(__LINE__);
    const u32 fixture=selected?choice[0]-L'0':0;const auto text=texts[fixture];
    const DWORD warm=GetEnvironmentVariableW(L"MAICORD_GLYPH_CACHE_PREWARM",choice,2);
    if(warm&&(warm!=1||choice[0]!=L'1'))return fail(__LINE__);
    ID3D11Query* complete=nullptr;
    struct ReleaseQuery {ID3D11Query*& value;~ReleaseQuery() noexcept {com_release(value);}} release_query{complete};
    if(warm){const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT,0};
        if(FAILED(renderer.graphics.device->CreateQuery(&description,&complete)))return fail(__LINE__);}
    const u32 repetitions[]{1,64,1,64,64};
    const wchar_t* names[]{L"glyph-cold.bmp",L"glyph-warm.bmp",L"glyph-reset.bmp",L"glyph-rewarm.bmp",L"glyph-retained.bmp"};
    u64 geometry=0;u32 quads=0;bool first_shape=true;u64 cold_ticks[2]{},cold_cycles[2]{};
    const auto draw=[&](bool shape) noexcept {
        if(shape){
            LARGE_INTEGER start{},end{};ULONG64 cycles=0,ended=0;
            if(first_shape&&(!QueryThreadCycleTime(GetCurrentThread(),&cycles)||!QueryPerformanceCounter(&start)))return fail(__LINE__);
            if(!text_shape_utf8(&owner.system,text,800,600,{owner.utf16,kRendererGlyphCapacity},&owner.title).ok())return fail(__LINE__);
            if(first_shape){if(!QueryPerformanceCounter(&end)||!QueryThreadCycleTime(GetCurrentThread(),&ended))return fail(__LINE__);
                cold_ticks[0]=end.QuadPart-start.QuadPart;cold_cycles[0]=ended-cycles;
                if(!QueryThreadCycleTime(GetCurrentThread(),&cycles)||!QueryPerformanceCounter(&start))return fail(__LINE__);}
            owner.title_quad_count=0;bool invalidated=false;
            ColrPaintBatch batch(context);owner.color_batch=&batch;
            struct Restore {D3D11GlyphTextOwner& owner;~Restore() noexcept {owner.color_batch=nullptr;}} restore{owner};
            D3D11GlyphTextAppendRequest request{};request.text=&owner.title;request.output=owner.title_quads;
            request.count=&owner.title_quad_count;request.atlas_invalidated=&invalidated;request.source=text;
            request.color={1,1,1,1};request.origin_x=16;request.origin_y=16;request.capacity=kRendererGlyphCapacity;request.dpi=renderer.dpi;
            if(!d3d11_glyph_text_append(&owner,context,request)||invalidated)return fail(__LINE__);
            batch.reset();
            content.quad_count=owner.title_quad_count;content_batch.quad_count=content.quad_count;++content.rebuilds;
            if(first_shape){if(!QueryPerformanceCounter(&end)||!QueryThreadCycleTime(GetCurrentThread(),&ended))return fail(__LINE__);
                cold_ticks[1]=end.QuadPart-start.QuadPart;cold_cycles[1]=ended-cycles;first_shape=false;
            }
        }
        const float clear[]{0,0,0,1};context->ClearRenderTargetView(renderer.graphics.target,clear);
        return d3d11_renderer_draw_stream(&renderer,kDefaultTheme,false,false,nullptr,10000)&&renderer.draw_calls_last_frame==1;
    };
    for(u32 phase=0;phase<5;++phase){
        if(phase&&warm){
            for(u32 i=0;i<4096;++i)if(!draw(false))return fail(__LINE__);
            context->End(complete);context->Flush();const u64 deadline=GetTickCount64()+250;
            for(;;){BOOL done=FALSE;const HRESULT status=context->GetData(complete,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if(status==S_OK&&done)break;if(FAILED(status)||GetTickCount64()>=deadline)return fail(__LINE__);Sleep(1);}
        }
        if(phase==2)d3d11_glyph_text_reset_atlas(&owner);
        if(!measurement.begin(renderer,phase))return fail(__LINE__);
        const u32 uploads=renderer.text_instance_uploads;
        for(u32 i=0;i<repetitions[phase];++i)if(!draw(phase!=4))return fail(__LINE__);
        if(!measurement.end(renderer))return fail(__LINE__);
        if(renderer.text_instance_uploads-uploads!=(phase==4?0:repetitions[phase]))return fail(__LINE__);
        const auto& record=measurement.pages[phase];
        if(phase==0||phase==2){if(!record.builds||
            (fixture!=2&&!record.mono_dispatches)||(fixture!=1&&!record.color_dispatches)||
            (fixture==2&&record.mono_dispatches)||(fixture==1&&record.color_dispatches))return fail(__LINE__);}
        else if(record.builds||record.mono_dispatches||record.color_dispatches||record.color_canceled_jobs)return fail(__LINE__);
        const u64 hash=hash64_bytes(hash64_bytes(kHash64Seed,owner.title.glyphs,owner.title.glyph_count*sizeof(TextGlyph)),owner.title.clusters,owner.title.cluster_count*2);
        if(!phase){geometry=hash;quads=owner.title_quad_count;}
        if(hash!=geometry||owner.title_quad_count!=quads||!quads)return fail(__LINE__);
        if(capture&&!capture(names[phase]))return fail(__LINE__);
    }
    if(!measurement.finish())return fail(__LINE__);
    log_u64(LogLevel::info,"glyph_cache","glyph_fixture",fixture);
    log_u64(LogLevel::info,"glyph_cache","glyph_prewarm_draws",warm?16384:0);
    log_u64(LogLevel::info,"glyph_cache","mono_rasters",owner.gpu.raster_count);
    log_u64(LogLevel::info,"glyph_cache","mono_submissions",owner.gpu.dispatch_count);
    log_u64(LogLevel::info,"glyph_cache","geometry_hash",geometry);
    log_u64(LogLevel::info,"glyph_cache","quad_hash",hash64_bytes(kHash64Seed,owner.title_quads,owner.title_quad_count*sizeof(GlyphQuad)));
    log_u64(LogLevel::info,"glyph_cache","glyphs",owner.title.glyph_count);
    log_u64(LogLevel::info,"glyph_cache","clusters",owner.title.cluster_count);
    log_u64(LogLevel::info,"glyph_cache","quads",quads);
    log_u64(LogLevel::info,"glyph_cache","cold_shape_ticks",cold_ticks[0]);
    log_u64(LogLevel::info,"glyph_cache","cold_shape_cycles",cold_cycles[0]);
    log_u64(LogLevel::info,"glyph_cache","cold_append_ticks",cold_ticks[1]);
    log_u64(LogLevel::info,"glyph_cache","cold_append_cycles",cold_cycles[1]);
    log_write(LogLevel::info,"probe","glyph_cache_passed");return true;
}
}
