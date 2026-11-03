#pragma once
#include "maicord/render/d3d11/renderer.hpp"
#include "glyph_gpu_footprint.hpp"
#include "maicord/core/log.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/process_info.hpp"
namespace maicord {
// QA only: per-page CPU time, memory and dispatch observations. Intervals include
// atlas generation, drawing and submission gaps; no GPU time is measured.
struct EmojiGpuMeasurement {
    struct Record {
        u64 cpu_ticks,thread_cycles;
        u64 local_usage,nonlocal_usage,local_budget,working_set,private_bytes;
        u64 textures,buffers,builds,mono_dispatches,color_dispatches,mono_pixels,mono_commands,row;
        u64 color_submissions,color_mask_submissions,color_canceled_jobs;
    };
    static_assert(sizeof(Record)==144);
    Record baseline{},pages[512]{};ID3D11Query* complete=nullptr;
    IDXGIAdapter3* adapter=nullptr;ID3D11DeviceContext* context=nullptr;
    wchar_t path[1024]{};u64 header[11]{};u32 count=0,capacity=0;bool enabled=false,active=false;
    LARGE_INTEGER cpu_begin{};ULONG64 cycle_begin=0;D3D11GlyphGpuFootprint previous{};u32 previous_builds=0;
    u32 page_control=0;u64 control_ticks=0;
    ~EmojiGpuMeasurement() noexcept {com_release(complete);com_release(adapter);}
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
        copy_bytes(&header[0],"MCETGPU3",8);header[1]=frequency.QuadPart;header[3]=sizeof(Record);
        header[4]=description.VendorId;header[5]=description.DeviceId;copy_bytes(&header[6],&description.AdapterLuid,8);
        header[7]=renderer.width;header[8]=renderer.height;header[9]=renderer.dpi;
        header[10]=3;
        header[10]|=static_cast<u64>(page_control)<<4;
        if(page_control==2){const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};if(FAILED(renderer.graphics.device->CreateQuery(&query,&complete)))return fail(__LINE__);}
        if(!snapshot(renderer,baseline))return fail(__LINE__);
        active=true;return true;
    }
    bool begin(D3D11Renderer& renderer,u32 row) noexcept {
        if(!enabled)return true;if(count==capacity||!active||!d3d11_glyph_text_gpu_footprint(renderer.glyph_text,&previous))return fail(__LINE__);
        previous_builds=renderer.glyph_text.cache_builds;pages[count].row=row;
        return QueryThreadCycleTime(GetCurrentThread(),&cycle_begin)&&QueryPerformanceCounter(&cpu_begin);
    }
    bool end(D3D11Renderer& renderer) noexcept {
        if(!enabled)return true;LARGE_INTEGER end{};ULONG64 cycles=0;D3D11GlyphGpuFootprint current{};
        if(!QueryPerformanceCounter(&end)||!QueryThreadCycleTime(GetCurrentThread(),&cycles)||
           end.QuadPart<cpu_begin.QuadPart||cycles<cycle_begin||!d3d11_glyph_text_gpu_footprint(renderer.glyph_text,&current))return fail(__LINE__);
        auto& row=pages[count++];row.cpu_ticks=end.QuadPart-cpu_begin.QuadPart;row.thread_cycles=cycles-cycle_begin;
        row.builds=renderer.glyph_text.cache_builds-previous_builds;row.mono_dispatches=current.mono_dispatches-previous.mono_dispatches;
        row.color_dispatches=current.color_dispatches-previous.color_dispatches;row.mono_pixels=current.mono_pixels-previous.mono_pixels;
        row.mono_commands=current.mono_commands-previous.mono_commands;
        if(page_control&&page_control!=3){LARGE_INTEGER start{},stop{};if(!QueryPerformanceCounter(&start))return fail(__LINE__);
            if(page_control==2)context->End(complete);context->Flush();
            if(page_control==2){const u64 deadline=GetTickCount64()+250;
                for(;;){BOOL done=FALSE;const HRESULT status=context->GetData(complete,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    if(status==S_OK&&done)break;if(FAILED(status)||GetTickCount64()>=deadline)return fail(__LINE__);Sleep(1);}}
            if(!QueryPerformanceCounter(&stop)||stop.QuadPart<start.QuadPart)return fail(__LINE__);control_ticks+=stop.QuadPart-start.QuadPart;}
        return snapshot(renderer,row);
    }
    bool finish() noexcept {
        if(!enabled)return true;if(!count||!active)return fail(__LINE__);active=false;context->Flush();header[2]=count;
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
