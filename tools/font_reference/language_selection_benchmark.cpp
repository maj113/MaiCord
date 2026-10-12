// QA only: compare current planners with the preceding first-party implementation.
#include "maicord/text/lookup_plan.hpp"
#include "maicord/text/arabic_plan.hpp"

#include <Windows.h>
namespace maicord {
SfntStatus previous_latin_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,bool,Slice<u64>,Slice<u16>,SfntLookupPlanResult*,u64) noexcept;
SfntStatus previous_common_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,Slice<u64>,Slice<u16>,SfntLookupPlanResult*,u64) noexcept;
SfntStatus previous_arabic_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,bool,Slice<u8>,Slice<SfntLookupStep>,SfntLookupPlanResult*,u64) noexcept;
namespace {
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}s[n]=0;print(s);}
bool argument(wchar_t* out,u32 size) noexcept {
    const wchar_t* p=GetCommandLineW();bool quote=false;while(*p){if(*p=='"')quote=!quote;else if(!quote&&(*p==' '||*p=='\t'))break;++p;}
    while(*p==' '||*p=='\t')++p;quote=*p=='"';if(quote)++p;u32 n=0;
    while(*p&&!(quote?*p=='"':(*p==' '||*p=='\t'))){if(n+1>=size)return false;out[n++]=*p++;}
    if(quote){if(*p!='"')return false;++p;}while(*p==' '||*p=='\t')++p;out[n]=0;return n&&!*p;
}
struct Storage {u64 bits[1024];u8 masks[65536];u16 indices[2048];SfntLookupStep steps[2048];SfntLookupPlanResult result;};
Storage current,previous;volatile u32 sink=0;u64 comparisons=0;
__declspec(noinline) SfntStatus execute(bool old,u32 profile,const SfntLayout& layout,Storage& s,u64 work=1000000,u32 capacity=2048) noexcept {
    if(profile<2)return old?previous_latin_lookup_plan(layout,{},profile!=0,{s.bits,1024},{s.indices,capacity},&s.result,work):sfnt_latin_lookup_plan(layout,{},profile!=0,{s.bits,1024},{s.indices,capacity},&s.result,work);
    if(profile==2)return old?previous_common_lookup_plan(layout,{},{s.bits,1024},{s.indices,capacity},&s.result,work):sfnt_common_lookup_plan(layout,{},{s.bits,1024},{s.indices,capacity},&s.result,work);
    return old?previous_arabic_lookup_plan(layout,{},profile==4,{s.masks,65536},{s.steps,capacity},&s.result,work):sfnt_arabic_lookup_plan(layout,{},profile==4,{s.masks,65536},{s.steps,capacity},&s.result,work);
}
bool compare(u32 profile,const SfntLayout& layout,u64 work,u32 capacity) noexcept {
    for(u32 i=0;i<2048;++i){current.indices[i]=previous.indices[i]=0xeeee;current.steps[i]=previous.steps[i]={0xeeee,0xee,0xee};}
    const auto a=execute(false,profile,layout,current,work,capacity),b=execute(true,profile,layout,previous,work,capacity);++comparisons;
    const auto& x=current.result;const auto& y=previous.result;
    return a==b&&x.operations==y.operations&&x.lookup_count==y.lookup_count&&x.script_tag==y.script_tag&&x.language_tag==y.language_tag&&x.language_found==y.language_found&&
        (profile<3?bytes_equal(current.indices,previous.indices,sizeof(current.indices)):bytes_equal(current.steps,previous.steps,sizeof(current.steps)));
}
bool benchmark(const SfntFace& face) noexcept {
    LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency))return false;
    for(u32 kind=0;kind<2;++kind){ConstSlice<u8> bytes{};const auto status=sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&bytes);
        if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return false;SfntLayout layout{};
        if(sfnt_layout_open(bytes,static_cast<SfntLayoutKind>(kind),&layout)!=SfntStatus::ok)return false;
        for(u32 profile=0;profile<5;++profile){if(execute(false,profile,layout,current)!=SfntStatus::ok)return false;
            const u32 selected=current.result.lookup_count;const u64 full=current.result.operations;
            for(u64 budget=0;budget<=full;++budget)if(!compare(profile,layout,budget,2048))return false;
            for(u32 capacity=0;capacity<=selected;++capacity)if(!compare(profile,layout,1000000,capacity))return false;
            for(u32 warm=0;warm<32;++warm)if(!compare(profile,layout,1000000,2048))return false;
            print("{\"kind\":");number(kind);print(",\"profile\":");number(profile);print(",\"selected\":");number(selected);print(",\"operations\":");number(full);
            print(",\"frequency\":");number(frequency.QuadPart);print(",\"iterations\":2048,\"batches\":[");
            for(u32 batch=0;batch<12;++batch){u64 ticks[2]{};for(u32 order=0;order<2;++order){const u32 version=(batch+order)&1;auto& s=version?previous:current;LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);
                    for(u32 repeat=0;repeat<2048;++repeat){if(execute(version!=0,profile,layout,s)!=SfntStatus::ok)return false;sink=sink+s.result.lookup_count;}
                    QueryPerformanceCounter(&end);ticks[version]=end.QuadPart-start.QuadPart;}
                if(batch)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print("]");}
            print("]}\n");}}
    print("comparisons=");number(comparisons);print(" PASS native language selection planner comparison\n");
    return !GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll");
}
int run() noexcept {
    wchar_t path[32768];if(!argument(path,32768))return 2;HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return 2;LARGE_INTEGER size{};bool okay=GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<0xffffffffll;
    HANDLE mapping=okay?CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr):nullptr;const auto* bytes=mapping?static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;
    SfntFace face{};okay=bytes&&sfnt_open({bytes,static_cast<u32>(size.QuadPart)},0,&face)==SfntStatus::ok&&benchmark(face);
    if(bytes)UnmapViewOfFile(bytes);if(mapping)CloseHandle(mapping);CloseHandle(file);return okay?0:1;
}
}}
extern "C" int maicord_language_selection_benchmark_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
