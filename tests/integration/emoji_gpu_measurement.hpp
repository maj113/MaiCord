#pragma once
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/log.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/process_info.hpp"
namespace maicord {
// QA only: device-timeline intervals include atlas generation, drawing and
// submission gaps. They are not hardware busy time or presentation latency.
struct EmojiGpuMeasurement {
    struct Record {
        u64 cpu_ticks,thread_cycles,gpu_ticks;
        u64 local_usage,nonlocal_usage,local_budget,working_set,private_bytes;
        u64 textures,buffers,builds,mono_dispatches,color_dispatches,mono_pixels,mono_commands,row;
        u64 color_submissions,color_mask_submissions,color_canceled_jobs;
    };
    static_assert(sizeof(Record)==152);
    Record baseline{},pages[512]{};ID3D11Query* begins[512]{};ID3D11Query* ends[512]{};ID3D11Query* disjoint=nullptr;
    IDXGIAdapter3* adapter=nullptr;ID3D11DeviceContext* context=nullptr;
    D3D11GpuTimerOwner* frame_timer=nullptr;bool frame_timer_enabled=false;
    wchar_t path[1024]{};u64 header[12]{};u32 count=0,capacity=0;bool enabled=false,active=false;
    LARGE_INTEGER cpu_begin{};ULONG64 cycle_begin=0;D3D11GlyphGpuFootprint previous{};u32 previous_builds=0;
    u32 page_control=0;u64 control_ticks=0;
    ~EmojiGpuMeasurement() noexcept {if(active&&context)context->End(disjoint);if(frame_timer)frame_timer->enabled=frame_timer_enabled;
        for(u32 i=0;i<capacity;++i){com_release(begins[i]);com_release(ends[i]);}com_release(disjoint);com_release(adapter);}
    static bool fail(u32 line) noexcept {log_u64(LogLevel::error,"probe","emoji_gpu_failure_line",line);return false;}
    bool snapshot(const D3D11Renderer& renderer,Record& row) noexcept {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{},remote{};PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
        D3D11GlyphGpuFootprint footprint{};
        if(FAILED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local))||
           FAILED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&remote))||
           !K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))||
           !d3d11_glyph_text_gpu_footprint(renderer.glyph_text,&footprint))return fail(__LINE__);
        row.local_usage=local.CurrentUsage;row.nonlocal_usage=remote.CurrentUsage;row.local_budget=local.Budget;
        row.working_set=memory.WorkingSetSize;row.private_bytes=memory.PrivateUsage;
        row.textures=footprint.texture_bytes;row.buffers=footprint.buffer_bytes;return true;
    }
    bool initialize(D3D11Renderer& renderer,u32 maximum_pages,const wchar_t* variable=L"MAICORD_EMOJI_GPU_METRICS") noexcept {
        const DWORD n=GetEnvironmentVariableW(variable,path,1024);if(!n)return true;
        if(n>=1024||!maximum_pages||maximum_pages>512)return fail(__LINE__);enabled=true;capacity=maximum_pages;context=renderer.graphics.context;
        wchar_t pacing[2]{};const DWORD setting=GetEnvironmentVariableW(L"MAICORD_EMOJI_PAGE_CONTROL",pacing,2);
        if(setting){if(setting!=1||pacing[0]<L'1'||pacing[0]>L'3')return fail(__LINE__);page_control=pacing[0]-L'0';}
        if(page_control==3&&renderer.live_resize)return fail(__LINE__);
        IDXGIDevice* dxgi=nullptr;IDXGIAdapter* base=nullptr;DXGI_ADAPTER_DESC description{};
        bool ok=SUCCEEDED(renderer.graphics.device->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(&dxgi)));
        if(ok)ok=SUCCEEDED(dxgi->GetAdapter(&base));
        if(ok)ok=SUCCEEDED(base->GetDesc(&description))&&SUCCEEDED(base->QueryInterface(__uuidof(IDXGIAdapter3),reinterpret_cast<void**>(&adapter)));
        com_release(base);com_release(dxgi);if(!ok)return fail(__LINE__);
        LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency))return fail(__LINE__);
        copy_bytes(&header[0],"MCETGPU2",8);header[1]=frequency.QuadPart;header[4]=sizeof(Record);
        header[5]=description.VendorId;header[6]=description.DeviceId;copy_bytes(&header[7],&description.AdapterLuid,8);
        header[8]=renderer.width;header[9]=renderer.height;header[10]=renderer.dpi;
        header[11]=3;
        header[11]|=static_cast<u64>(page_control)<<4;
        D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT,0};if(FAILED(renderer.graphics.device->CreateQuery(&query,&disjoint)))return fail(__LINE__);
        query.Query=D3D11_QUERY_TIMESTAMP;
        for(u32 i=0;i<capacity;++i)if(FAILED(renderer.graphics.device->CreateQuery(&query,&begins[i]))||FAILED(renderer.graphics.device->CreateQuery(&query,&ends[i])))return fail(__LINE__);
        if(!snapshot(renderer,baseline))return fail(__LINE__);
        frame_timer=&renderer.gpu_timer;frame_timer_enabled=frame_timer->enabled;frame_timer->enabled=false;
        context->Begin(disjoint);active=true;return true;
    }
    bool begin(D3D11Renderer& renderer,u32 row) noexcept {
        if(!enabled)return true;if(count==capacity||!active||!d3d11_glyph_text_gpu_footprint(renderer.glyph_text,&previous))return fail(__LINE__);
        previous_builds=renderer.glyph_text.cache_builds;pages[count].row=row;
        context->End(begins[count]);return QueryThreadCycleTime(GetCurrentThread(),&cycle_begin)&&QueryPerformanceCounter(&cpu_begin);
    }
    bool end(D3D11Renderer& renderer) noexcept {
        if(!enabled)return true;context->End(ends[count]);LARGE_INTEGER end{};ULONG64 cycles=0;D3D11GlyphGpuFootprint current{};
        if(!QueryPerformanceCounter(&end)||!QueryThreadCycleTime(GetCurrentThread(),&cycles)||
           end.QuadPart<cpu_begin.QuadPart||cycles<cycle_begin||!d3d11_glyph_text_gpu_footprint(renderer.glyph_text,&current))return fail(__LINE__);
        auto& row=pages[count++];row.cpu_ticks=end.QuadPart-cpu_begin.QuadPart;row.thread_cycles=cycles-cycle_begin;
        row.builds=renderer.glyph_text.cache_builds-previous_builds;row.mono_dispatches=current.mono_dispatches-previous.mono_dispatches;
        row.color_dispatches=current.color_dispatches-previous.color_dispatches;row.mono_pixels=current.mono_pixels-previous.mono_pixels;
        row.mono_commands=current.mono_commands-previous.mono_commands;
        if(page_control&&page_control!=3){LARGE_INTEGER start{},stop{};if(!QueryPerformanceCounter(&start))return fail(__LINE__);context->Flush();
            if(page_control==2){const u64 deadline=GetTickCount64()+250;
                for(;;){u64 timestamp=0;const HRESULT status=context->GetData(ends[count-1],&timestamp,sizeof(timestamp),D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    if(status==S_OK)break;if(FAILED(status)||GetTickCount64()>=deadline)return fail(__LINE__);Sleep(1);}}
            if(!QueryPerformanceCounter(&stop)||stop.QuadPart<start.QuadPart)return fail(__LINE__);control_ticks+=stop.QuadPart-start.QuadPart;}
        return snapshot(renderer,row);
    }
    bool finish() noexcept {
        if(!enabled)return true;if(!count||!active)return fail(__LINE__);context->End(disjoint);active=false;context->Flush();
        // Disjoint readiness does not imply every timestamp result is visible.
        // All retries share one fixed deadline; no per-query timeout extension.
        const u64 limit=GetTickCount64()+250;
        const auto read=[&](ID3D11Query* query,void* value,u32 bytes) noexcept {
            for(;;){const HRESULT result=context->GetData(query,value,bytes,D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if(result!=S_FALSE)return result==S_OK;if(GetTickCount64()>=limit)return false;Sleep(1);}};
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
        if(!read(disjoint,&data,sizeof(data))||data.Disjoint||!data.Frequency)return fail(__LINE__);header[2]=data.Frequency;header[3]=count;
        for(u32 i=0;i<count;++i){u64 first=0,last=0;
            if(!read(begins[i],&first,sizeof(first))||!read(ends[i],&last,sizeof(last))||last<first)return fail(__LINE__);
            pages[i].gpu_ticks=last-first;}
        if(frame_timer){frame_timer->enabled=frame_timer_enabled;frame_timer=nullptr;}
        HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return fail(__LINE__);DWORD written=0;
        bool ok=WriteFile(file,header,sizeof(header),&written,nullptr)&&written==sizeof(header);
        if(ok)ok=WriteFile(file,&baseline,sizeof(baseline),&written,nullptr)&&written==sizeof(baseline);
        if(ok)ok=WriteFile(file,pages,count*sizeof(Record),&written,nullptr)&&written==count*sizeof(Record);
        const BOOL closed=CloseHandle(file);
        if(ok&&closed&&page_control){log_u64(LogLevel::info,"probe","emoji_page_control_count",count);
            log_u64(LogLevel::info,"probe","emoji_page_control_us",control_ticks*1000000/header[1]);}
        return ok&&closed;
    }
};
}
