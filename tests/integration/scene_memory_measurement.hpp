#pragma once
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/log.hpp"
#include "maicord/sys/dxgi.hpp"
#include "maicord/sys/process_info.hpp"

namespace maicord {
// Ordinary QA snapshots add no GPU wait, draw, capture texture or timestamp
// query. Whole-process usage
// includes the driver and this instrumentation; descriptor bytes are payload.
struct SceneMemoryMeasurement {
    struct Record { wchar_t name[64]{}; u64 values[20]{}; };
    static_assert(sizeof(Record)==288);
    HANDLE file=INVALID_HANDLE_VALUE;
    IDXGIAdapter3* adapter=nullptr;
    u64 header[8]{};
    bool failed=false;
    bool font_residency=false;
    bool enabled() const noexcept {return file!=INVALID_HANDLE_VALUE;}

    bool fail() noexcept {failed=true;log_write(LogLevel::error,"probe","scene_memory_failed");return false;}
    bool write(const void* data,u32 size) noexcept {
        DWORD written=0;
        return WriteFile(file,data,size,&written,nullptr)&&written==size;
    }
    bool initialize(const D3D11Renderer& renderer) noexcept {
        wchar_t path[1024]{};
        const DWORD n=GetEnvironmentVariableW(L"MAICORD_SCENE_MEMORY",path,1024);
        if(!n)return true;
        if(n>=1024)return fail();
        wchar_t drain[2]{};
        font_residency=GetEnvironmentVariableW(L"MAICORD_SCENE_FONT_RESIDENCY",drain,2)==1&&drain[0]==L'1';
        IDXGIDevice* dxgi=nullptr;IDXGIAdapter* base=nullptr;DXGI_ADAPTER_DESC description{};
        bool ok=SUCCEEDED(renderer.graphics.device->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(&dxgi)));
        if(ok)ok=SUCCEEDED(dxgi->GetAdapter(&base));
        if(ok)ok=SUCCEEDED(base->GetDesc(&description))&&
            SUCCEEDED(base->QueryInterface(__uuidof(IDXGIAdapter3),reinterpret_cast<void**>(&adapter)));
        com_release(base);com_release(dxgi);
        if(!ok)return fail();
        copy_bytes(header,"MCMSCN02",8);header[1]=sizeof(Record);
        header[3]=description.VendorId;header[4]=description.DeviceId;
        copy_bytes(&header[5],&description.AdapterLuid,8);
        header[2]=3;
        file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE||!write(header,sizeof(header)))return fail();
        return snapshot(L"renderer-initialized",&renderer);
    }
    bool snapshot(const wchar_t* name,const D3D11Renderer* renderer) noexcept {
        if(failed)return false;
        if(file==INVALID_HANDLE_VALUE)return true;
        Record record{};u32 n=0;
        for(;name[n]&&n<63;++n)record.name[n]=name[n];
        if(name[n])return fail();
        DXGI_QUERY_VIDEO_MEMORY_INFO local{},remote{};
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
        if(FAILED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local))||
           FAILED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&remote))||
           !K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))return fail();
        auto* v=record.values;
        v[3]=local.CurrentUsage;v[4]=remote.CurrentUsage;v[5]=local.Budget;
        v[6]=memory.WorkingSetSize;v[7]=memory.PrivateUsage;
        if(renderer){D3D11GlyphGpuFootprint footprint{};
            if(!d3d11_glyph_text_gpu_footprint(renderer->glyph_text,&footprint))return fail();
            v[0]=renderer->width;v[1]=renderer->height;v[2]=renderer->dpi;
            v[8]=footprint.texture_bytes;v[9]=footprint.buffer_bytes;
            v[10]=renderer->glyph_text.cache_builds;v[11]=footprint.mono_dispatches;
            v[12]=footprint.color_dispatches;v[13]=footprint.mono_pixels;v[14]=footprint.mono_commands;
            v[15]=renderer->glyph_text.title_quad_count;
        }
        if(font_residency&&!font_pages(CompareStringOrdinal(name,-1,L"font-16.bmp",-1,FALSE)==CSTR_EQUAL))return fail();
        if(!write(&record,sizeof(record)))return fail();
        ++header[6];return true;
    }
    bool font_pages(bool details) noexcept {
        SYSTEM_INFO info{};GetSystemInfo(&info);
        const usize limit=reinterpret_cast<usize>(info.lpMaximumApplicationAddress),page=info.dwPageSize;
        if(!page)return false;
        static wchar_t path[32768];PSAPI_WORKING_SET_EX_INFORMATION pages[256]{};
        u64 mapped=0,resident=0,private_resident=0;usize at=0;
        while(at<=limit){MEMORY_BASIC_INFORMATION region{};
            if(VirtualQuery(reinterpret_cast<const void*>(at),&region,sizeof(region))!=sizeof(region))return false;
            const usize begin=reinterpret_cast<usize>(region.BaseAddress),end=begin+region.RegionSize;
            if(end<=at)return false;
            if(region.State==MEM_COMMIT&&region.Type==MEM_MAPPED){
                const DWORD length=K32GetMappedFileNameW(GetCurrentProcess(),region.BaseAddress,path,32768);
                if(length>=32768)return false;
                if(length>=4&&path[length-4]==L'.'&&((path[length-3]|32)==L't'||(path[length-3]|32)==L'o')&&
                    (path[length-2]|32)==L't'&&((path[length-1]|32)==L'f'||(path[length-1]|32)==L'c')){
                    mapped+=region.RegionSize;
                    u64 region_resident=0,region_private=0;
                    for(usize address=begin;address<end;){u32 count=0;
                        while(count<256&&address<end){pages[count++].VirtualAddress=reinterpret_cast<void*>(address);address+=page;}
                        if(!K32QueryWorkingSetEx(GetCurrentProcess(),pages,static_cast<DWORD>(count*sizeof(pages[0]))))return false;
                        for(u32 i=0;i<count;++i)if(pages[i].VirtualAttributes.Valid){region_resident+=page;if(!pages[i].VirtualAttributes.Shared)region_private+=page;}
                    }
                    resident+=region_resident;private_resident+=region_private;
                    if(details){u32 first=0;for(u32 i=0;i<length;++i)if(path[i]==L'\\'||path[i]==L'/')first=i+1;
                        constexpr char prefix[]="font_page_file=";char label[1040];copy_bytes(label,prefix,sizeof(prefix)-1);
                        const int bytes=WideCharToMultiByte(CP_UTF8,0,path+first,static_cast<int>(length-first),label+sizeof(prefix)-1,1024,nullptr,nullptr);
                        if(bytes<=0)return false;label[sizeof(prefix)-1+bytes]=0;
                        log_write(LogLevel::info,"probe",label);
                        log_u64(LogLevel::info,"probe","font_page_mapping_bytes",region.RegionSize);
                        log_u64(LogLevel::info,"probe","font_page_resident_bytes",region_resident);
                        log_u64(LogLevel::info,"probe","font_page_private_bytes",region_private);
                    }
                }
            }at=end;
        }
        log_u64(LogLevel::info,"probe","font_file_mapped_bytes",mapped);
        log_u64(LogLevel::info,"probe","font_file_resident_bytes",resident);
        log_u64(LogLevel::info,"probe","font_file_private_resident_bytes",private_resident);
        return private_resident<=resident&&resident<=mapped;
    }
    ~SceneMemoryMeasurement() noexcept {
        if(file!=INVALID_HANDLE_VALUE){
            if(!failed)snapshot(L"probe-scope-end",nullptr);
            LARGE_INTEGER start{};
            const bool ok=!failed&&header[6]&&SetFilePointerEx(file,start,nullptr,FILE_BEGIN)&&write(header,sizeof(header));
            const BOOL closed=CloseHandle(file);
            log_write(ok&&closed?LogLevel::info:LogLevel::error,"probe",ok&&closed?"scene_memory_passed":"scene_memory_failed");
        }
        com_release(adapter);
    }
};
}
