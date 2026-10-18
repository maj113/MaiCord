#pragma once

// Test-only Windows 10 diagnostics. No private NT ABI, global handle table,
// page contents, certificates, HTTP data, or arbitrary object paths are read.
// PSS schemas/lifetimes: Microsoft Learn processsnapshot/PSS_HANDLE_ENTRY and
// PSS_HANDLE_FLAGS. PSS_CREATE_USE_VM_ALLOCATIONS avoids the process heap.
#include "maicord/sys/process_info.hpp"
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif

namespace maicord { namespace resource_probe {
constexpr u32 kHandles = 1024, kModules = 256, kName = 192;
void out(const char* value) noexcept {DWORD bytes=0;while(value[bytes])++bytes;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),value,bytes,&done,nullptr);}
void num(u64 value) noexcept {char text[32];u32 n=0;do{text[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char c=text[i];text[i]=text[n-i-1];text[n-i-1]=c;}DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&done,nullptr);}
void field(const char* name,u64 value) noexcept {out(" ");out(name);out("=");num(value);}
struct Handle {usize value=0;u64 name_id=0,type_id=0;char type[48]{},name[kName]{};u32 flags=0,thread_id=0;};
struct Module {HMODULE base=nullptr;DWORD image_bytes=0;char name[96]{};};
struct Snapshot {Handle handles[kHandles]{};Module modules[kModules]{};u32 count=0,module_count=0;bool valid=false;};
// All storage is prefaulted before baseline. Fingerprints remain in memory;
// output uses safe path classes/public certificate-store suffixes only.
inline Snapshot snapshots[5]{};
inline u32 previous_slot=0;
// Optional observer control only. These records describe our own walker
// allocations; they never dereference an enumerated process/private heap.
constexpr u32 kCallbackOwners=64;
struct CallbackOwner {void* address=nullptr;u32 requested=0;usize committed=0;};
inline CallbackOwner callback_owners[kCallbackOwners]{};
inline bool control_active=false;
inline u32 control_index=0,callback_failures=0;
inline u64 callback_allocations=0,callback_releases=0,callback_requested=0,callback_committed=0;
using HeapSummaryFunction=BOOL (WINAPI*)(HANDLE,DWORD,LPHEAP_SUMMARY);
inline HeapSummaryFunction heap_summary=nullptr;
wchar_t lower(wchar_t c) noexcept {return c>=L'A'&&c<=L'Z'?static_cast<wchar_t>(c+32):c;}
u64 fingerprint(const wchar_t* text,u32 chars) noexcept {u64 h=14695981039346656037ull;for(u32 i=0;text&&i<chars;++i){h^=static_cast<u64>(text[i]);h*=1099511628211ull;}return h;}
bool prefix(const wchar_t* text,u32 chars,const wchar_t* pattern) noexcept {u32 i=0;while(pattern[i]){if(i>=chars||lower(text[i])!=lower(pattern[i]))return false;++i;}return true;}
void copy(char* dest,u32 capacity,const char* text) noexcept {u32 i=0;while(text[i]&&i+1<capacity){dest[i]=text[i];++i;}dest[i]=0;}
void ascii(char* dest,u32 capacity,const wchar_t* text,u32 chars) noexcept {u32 i=0;for(;text&&i<chars&&i+1<capacity&&text[i];++i){const wchar_t c=text[i];dest[i]=c>=32&&c<=126?static_cast<char>(c):'?';}dest[i]=0;}
void safe_name(char* dest,const wchar_t* text,u32 chars,bool known) noexcept {
    if(!known){copy(dest,kName,"name-unavailable");return;}
    if(!chars){copy(dest,kName,"unnamed");return;}
    // These are public Windows certificate registry stores, not certificate
    // data/private keys. Strip machine/user prefixes (including user SIDs).
    constexpr wchar_t marker[]=L"\\SystemCertificates\\";
    const bool registry=prefix(text,chars,L"\\REGISTRY\\MACHINE\\")||prefix(text,chars,L"\\REGISTRY\\USER\\");
    for(u32 i=0;registry&&i<chars;++i)if(prefix(text+i,chars-i,marker)){
        copy(dest,kName,"SystemCertificates/");u32 n=19;
        for(u32 j=i+20;j<chars&&n+1<kName&&text[j];++j){const wchar_t c=text[j];const bool safe=(c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'\\'||c==L'_'||c==L'-'||c==L'.';dest[n++]=safe?static_cast<char>(c):'?';}dest[n]=0;return;
    }
    const char* category="named-redacted";
    if(prefix(text,chars,L"\\Device\\Afd"))category="Device/Afd";
    else if(prefix(text,chars,L"\\Device\\Tcp"))category="Device/Tcp";
    else if(prefix(text,chars,L"\\Device\\Nsi"))category="Device/Nsi";
    else if(prefix(text,chars,L"\\Device\\KsecDD"))category="Device/KsecDD";
    else if(prefix(text,chars,L"\\REGISTRY\\MACHINE\\"))category="registry-machine-other";
    else if(prefix(text,chars,L"\\REGISTRY\\USER\\"))category="registry-user-other";
    else if(prefix(text,chars,L"\\RPC Control\\"))category="RPC-Control";
    else if(prefix(text,chars,L"\\BaseNamedObjects\\"))category="BaseNamedObjects";
    copy(dest,kName,category);
}
void* WINAPI allocate(void*,DWORD bytes) noexcept {
    if(bytes>1024u*1024u){if(control_active)++callback_failures;return nullptr;}
    if(!control_active)return VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    u32 slot=0;while(slot<kCallbackOwners&&callback_owners[slot].address)++slot;
    if(slot==kCallbackOwners){++callback_failures;return nullptr;}
    void* address=VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!address){++callback_failures;return nullptr;}
    MEMORY_BASIC_INFORMATION region{};
    if(VirtualQuery(address,&region,sizeof(region))!=sizeof(region)||region.State!=MEM_COMMIT){
        ++callback_failures;VirtualFree(address,0,MEM_RELEASE);return nullptr;
    }
    callback_owners[slot]={address,bytes,region.RegionSize};
    ++callback_allocations;callback_requested+=bytes;callback_committed+=region.RegionSize;
    return address;
}
void WINAPI release(void*,void* address) noexcept {
    if(!address)return;
    if(!control_active){VirtualFree(address,0,MEM_RELEASE);return;}
    u32 slot=0;while(slot<kCallbackOwners&&callback_owners[slot].address!=address)++slot;
    if(slot==kCallbackOwners){++callback_failures;return;}
    if(!VirtualFree(address,0,MEM_RELEASE)){++callback_failures;return;}
    callback_requested-=callback_owners[slot].requested;callback_committed-=callback_owners[slot].committed;
    callback_owners[slot]={};++callback_releases;
}
bool same(const Handle& a,const Handle& b) noexcept {return a.value==b.value&&a.type_id==b.type_id&&a.name_id==b.name_id&&a.thread_id==b.thread_id;}
bool contains(const Snapshot& snap,const Handle& handle) noexcept {for(u32 i=0;i<snap.count;++i)if(same(snap.handles[i],handle))return true;return false;}
void row(const char* change,const Handle& h) noexcept {out("resource_handle change=");out(change);field("value",h.value);out(" type=");out(h.type);out(" name=");out(h.name);field("flags",h.flags);if(h.thread_id)field("thread_id",h.thread_id);out("\n");}
bool memory() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
    const bool ok=K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))!=FALSE;
    out("resource_memory");field("ok",ok?1:0);field("working_set",counters.WorkingSetSize);field("private_bytes",counters.PrivateUsage);field("pagefile_bytes",counters.PagefileUsage);
    field("paged_pool",counters.QuotaPagedPoolUsage);field("nonpaged_pool",counters.QuotaNonPagedPoolUsage);out("\n");
    SYSTEM_INFO system{};GetSystemInfo(&system);usize address=0;
    const usize maximum_address=reinterpret_cast<usize>(system.lpMaximumApplicationAddress);
    u64 private_commit=0,image_commit=0,mapped_commit=0,private_reserved=0;u32 regions=0;bool complete=false;
    // Metadata only; never reads page contents. A fixed region bound fails
    // visibly rather than walking an unexpected large process indefinitely.
    for(;regions<8192&&address<=maximum_address;++regions){
        MEMORY_BASIC_INFORMATION region{};
        if(VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region))!=sizeof(region))break;
        if(region.Type==MEM_PRIVATE){if(region.State==MEM_COMMIT)private_commit+=region.RegionSize;else if(region.State==MEM_RESERVE)private_reserved+=region.RegionSize;}
        if(region.State==MEM_COMMIT&&region.Type==MEM_IMAGE)image_commit+=region.RegionSize;
        if(region.State==MEM_COMMIT&&region.Type==MEM_MAPPED)mapped_commit+=region.RegionSize;
        const usize next=reinterpret_cast<usize>(region.BaseAddress)+region.RegionSize;
        if(next<=address||next>maximum_address){complete=true;break;}address=next;
    }
    out("resource_va");field("complete",complete?1:0);field("regions",regions);field("private_commit",private_commit);field("private_reserved",private_reserved);field("image_commit",image_commit);field("mapped_commit",mapped_commit);out("\n");
    return ok&&complete;
}
bool control_measure(const char* stage) noexcept {
    LARGE_INTEGER observed{};QueryPerformanceCounter(&observed);
    DWORD handles=0;const bool handles_ok=GetProcessHandleCount(GetCurrentProcess(),&handles)!=FALSE;
    out("resource_observer");field("sample_index",control_index);out(" stage=");out(stage);
    field("qpc_ticks",static_cast<u64>(observed.QuadPart));
    field("handles_ok",handles_ok?1:0);field("handles",handles);
    field("callback_allocations",callback_allocations);field("callback_releases",callback_releases);
    field("callback_outstanding_requested",callback_requested);field("callback_outstanding_commit",callback_committed);
    field("callback_failures",callback_failures);out("\n");
    const bool memory_ok=memory();
    if(heap_summary){
        HEAP_SUMMARY summary{};summary.cb=sizeof(summary);
        LARGE_INTEGER begin{},end{};QueryPerformanceCounter(&begin);
        const bool ok=heap_summary(GetProcessHeap(),0,&summary)!=FALSE;
        const DWORD error=ok?ERROR_SUCCESS:GetLastError();QueryPerformanceCounter(&end);
        out("resource_default_heap");field("sample_index",control_index);out(" stage=");out(stage);
        field("ok",ok?1:0);field("error",error);field("allocated",summary.cbAllocated);
        field("committed",summary.cbCommitted);field("reserved",summary.cbReserved);field("max_reserved",summary.cbMaxReserve);
        field("query_ticks",static_cast<u64>(end.QuadPart-begin.QuadPart));out("\n");
    }
    return handles_ok&&memory_ok&&!callback_failures&&!callback_requested&&!callback_committed;
}
bool capture(Snapshot& dest) noexcept {
    dest.count=0;dest.module_count=0;dest.valid=false;
    DWORD before=0;if(!GetProcessHandleCount(GetCurrentProcess(),&before)||before>kHandles-64){out("resource_capture_failed handle_count_cap\n");return false;}
    HPSS snap=nullptr;HPSSWALK walk=nullptr;
    const auto flags=static_cast<PSS_CAPTURE_FLAGS>(PSS_CAPTURE_HANDLES|PSS_CAPTURE_HANDLE_NAME_INFORMATION|PSS_CAPTURE_HANDLE_BASIC_INFORMATION|PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION|PSS_CREATE_USE_VM_ALLOCATIONS);
    DWORD error=PssCaptureSnapshot(GetCurrentProcess(),flags,0,&snap);
    PSS_ALLOCATOR allocator{nullptr,allocate,release};
    if(error==ERROR_SUCCESS)error=PssWalkMarkerCreate(&allocator,&walk);
    while(error==ERROR_SUCCESS&&dest.count<kHandles){
        PSS_HANDLE_ENTRY entry{};error=PssWalkSnapshot(snap,PSS_WALK_HANDLES,walk,&entry,sizeof(entry));
        if(error!=ERROR_SUCCESS)break;
        auto& h=dest.handles[dest.count++];h=Handle{};h.value=reinterpret_cast<usize>(entry.Handle);h.flags=static_cast<u32>(entry.Flags);
        const u32 type_chars=static_cast<u32>(entry.TypeNameLength/sizeof(wchar_t)),name_chars=static_cast<u32>(entry.ObjectNameLength/sizeof(wchar_t));
        h.type_id=fingerprint(entry.TypeName,type_chars);h.name_id=fingerprint(entry.ObjectName,name_chars);
        ascii(h.type,sizeof(h.type),entry.TypeName,type_chars);
        if(!h.type[0])copy(h.type,sizeof(h.type),"type-unavailable");
        safe_name(h.name,entry.ObjectName,name_chars,(entry.Flags&PSS_HANDLE_HAVE_NAME)!=0);
        if((entry.Flags&PSS_HANDLE_HAVE_TYPE_SPECIFIC_INFORMATION)&&entry.ObjectType==PSS_OBJECT_TYPE_THREAD)h.thread_id=entry.TypeSpecificInformation.Thread.ThreadId;
    }
    bool ok=error==ERROR_NO_MORE_ITEMS;
    if(walk&&PssWalkMarkerFree(walk)!=ERROR_SUCCESS)ok=false;
    if(snap&&PssFreeSnapshot(GetCurrentProcess(),snap)!=ERROR_SUCCESS)ok=false;
    out("resource_capture");field("pre_handles",before);field("entries",dest.count);field("status",error);field("ok",ok?1:0);out("\n");
    if(control_active)ok=control_measure("after_pss_free")&&ok;
    HMODULE modules[kModules]{};DWORD needed=0;
    if(!K32EnumProcessModules(GetCurrentProcess(),modules,sizeof(modules),&needed)||needed>sizeof(modules))ok=false;
    else for(u32 i=0;i<needed/sizeof(HMODULE);++i){
        MODULEINFO info{};wchar_t name[96]{};
        if(!K32GetModuleInformation(GetCurrentProcess(),modules[i],&info,sizeof(info))||!K32GetModuleBaseNameW(GetCurrentProcess(),modules[i],name,96)){ok=false;break;}
        auto& module=dest.modules[dest.module_count++];module.base=modules[i];module.image_bytes=info.SizeOfImage;
        u32 chars=0;while(chars<96&&name[chars])++chars;ascii(module.name,sizeof(module.name),name,chars);
    }
    if(control_active)ok=control_measure("after_modules")&&ok;
    dest.valid=ok;return ok;
}
bool sample(const char* phase,u32 slot,bool report_rows=true) noexcept {
    out("resource_phase=");out(phase);out("\n");
    // Sample process memory before the temporary snapshot allocations.
    const bool memory_ok=control_active?control_measure("before_capture"):memory();Snapshot& now=snapshots[slot];const Snapshot& before=snapshots[previous_slot];
    const bool ok=capture(now)&&memory_ok;
    if(ok&&before.valid&&slot!=previous_slot){
        u32 added=0,removed=0;
        for(u32 i=0;i<now.count;++i)if(!contains(before,now.handles[i])){++added;if(report_rows)row("appeared",now.handles[i]);}
        for(u32 i=0;i<before.count;++i)if(!contains(now,before.handles[i])){++removed;if(report_rows)row("disappeared",before.handles[i]);}
        out("resource_delta");field("added",added);field("removed",removed);out("\n");
        for(u32 i=0;i<now.module_count;++i){bool found=false;for(u32 j=0;j<before.module_count;++j)found|=now.modules[i].base==before.modules[j].base;
            if(!found){out("resource_module appeared=");out(now.modules[i].name);field("image_bytes",now.modules[i].image_bytes);out("\n");}}
    }
    previous_slot=slot;if(control_active)++control_index;return ok;
}
void prefault() noexcept {
    auto* bytes=reinterpret_cast<volatile u8*>(snapshots);for(usize i=0;i<sizeof(snapshots);i+=4096)bytes[i]=0;
    bytes[sizeof(snapshots)-1]=0;
    auto* owner_bytes=reinterpret_cast<volatile u8*>(callback_owners);for(usize i=0;i<sizeof(callback_owners);i+=4096)owner_bytes[i]=0;
    owner_bytes[sizeof(callback_owners)-1]=0;
    out("resource_identity_note=handle_value_type_name_match_does_not_prove_object_identity_or_unbroken_lifetime\n");
    out("resource_cost_note=module_image_bytes_and_va_commit_are_not_resident_bytes;working_set_is_process_total;instrumentation_storage_prefaulted\n");
}
bool initialize() noexcept {
    prefault();
    const bool first=sample("instrumentation_first",0,false),second=sample("instrumentation_second",1);
    return first&&second;
}
void initialize_control(bool summarize_heap=true) noexcept {
    prefault();control_active=true;
    const HMODULE kernel=GetModuleHandleW(L"kernel32.dll");
    const auto available=kernel?reinterpret_cast<HeapSummaryFunction>(GetProcAddress(kernel,"HeapSummary")):nullptr;
    heap_summary=summarize_heap?available:nullptr;
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    out("resource_control_note=idle_observer_control_then_one_cold_client_then_idle_observer_control;no_warm_clients\n");
    out("resource_heap_summary_capability");field("documented_min_build",20348);field("available",available?1:0);field("enabled",summarize_heap?1:0);
    field("default_heap_only",1);field("qpc_frequency",static_cast<u64>(frequency.QuadPart));out("\n");
}
struct Owner {HANDLE handles[4]{};void* runtime=nullptr;};
Owner owner(const CdnClient& client) noexcept {return {{client.response_event,client.worker.thread,client.worker.ready_event,client.worker.loop.completion_port},client.runtime};}
bool owner_released(const Owner& owned) noexcept {
    u32 live=0;for(u32 i=0;i<4;++i)if(owned.handles[i]){DWORD flags=0;if(GetHandleInformation(owned.handles[i],&flags))live|=1u<<i;}
    MEMORY_BASIC_INFORMATION region{};bool freed=!owned.runtime;
    if(owned.runtime)freed=VirtualQuery(owned.runtime,&region,sizeof(region))==sizeof(region)&&region.State==MEM_FREE;
    out("resource_owner_release");field("old_native_handles_still_valid_mask",live);field("runtime_address_free",freed?1:0);out("\n");
    return !live&&freed;
}
void retained_cold() noexcept {
    const Snapshot& baseline=snapshots[4];const Snapshot& cold=snapshots[2];const Snapshot& warm=snapshots[3];
    u32 matching=0,new_cold=0,new_retained=0;
    for(u32 i=0;i<cold.count;++i){const auto& h=cold.handles[i];const bool retained=contains(warm,h);if(retained)++matching;
        if(!contains(baseline,h)){++new_cold;if(retained)++new_retained;row(retained?"cold_new_matching_warm_stop":"cold_new_missing_warm_stop",h);}}
    out("resource_cold_stop_vs_warm_stop");field("cold_count",cold.count);field("warm_count",warm.count);field("same_handle_type_name_slots",matching);field("new_cold_slots",new_cold);field("new_cold_slots_matching_warm",new_retained);out("\n");
}
}}
