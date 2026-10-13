#include "maicord/text/font_fallback_range.hpp"
#include <Windows.h>
namespace maicord {namespace {
u32 checks=0,classes=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u32 v) noexcept {char s[16],out[16];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n;++i)out[i]=s[n-i-1];out[n]=0;print(out);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print(" scalar=");number(classes);print("\n");return false;}}while(0)
struct Mock {u32 selects=0,probes=0,fail=0;bool cover=true;};
SfntStatus select(void* context,u32 cp,NativeFallbackChoice* out) noexcept {auto& m=*static_cast<Mock*>(context);++m.selects;if(m.fail==1)return SfntStatus::malformed;*out={cp=='b'?1u:cp=='?'?native_fallback_missing:0u,cp=='x'?1u:0u};return SfntStatus::ok;}
SfntStatus coverage(void* context,u32,u32,bool* out) noexcept {auto& m=*static_cast<Mock*>(context);++m.probes;if(m.fail==2)return SfntStatus::truncated;*out=m.cover;return SfntStatus::ok;}
bool empty(const NativeFallbackRangeResult& r) noexcept {return !r.bytes&&!r.units&&r.choice.font==native_fallback_missing&&r.choice.policy==native_fallback_missing;}
struct RoutingMock {Mock font;u32 routes=0;bool fail_route=false;};
SfntStatus route_index(void* context,u32 cp,u32* out) noexcept {auto& m=*static_cast<RoutingMock*>(context);++m.routes;if(m.fail_route)return SfntStatus::truncated;*out=cp=='x'?1u:0u;return SfntStatus::ok;}
SfntStatus font_index(void* context,u32 cp,u32* out) noexcept {NativeFallbackChoice choice{};const auto status=select(&static_cast<RoutingMock*>(context)->font,cp,&choice);*out=choice.font;return status;}
SfntStatus routed_coverage(void* context,u32 font,u32 cp,bool* out) noexcept {return coverage(&static_cast<RoutingMock*>(context)->font,font,cp,out);}
bool routing_guards() noexcept {
    struct Case {const char* text;u32 bytes,units,font,probes;};
    const Case cases[]{ {"a\xee\x80\x80",1,1,0,0},{"\xee\x80\x80" "a",3,1,0,0},
        {"a\xef\xb8\x8f" "a",5,3,0,0},{"a\xef\xb8\x8f\xef\xb8\x8f" "a",8,4,0,0},
        {"a\xcd\x8f" "x",4,3,0,2},{"a\xcd\x8f\xef\xb8\x8f" "x",7,4,0,2},
        {"b\xcd\x8f\xef\xb8\x8f" "a",6,3,1,1},{"b\xcd\x8f" "a",4,3,1,2},
        {"a\xcd\x8f\xef\xb8\x8f\xef\xb8\x8f" "x",9,4,0,1},
        {"a\xf3\xa0\x84\x80" "x",5,3,0,0},{"?\xef\xb8\x8f\xef\xb8\x8f" "a",1,1,native_fallback_missing,0},
        {"\xef\xb8\x8f\xe1\xa0\x8b",6,2,0,0} };
    NativeFallbackRangeResult result{};u32 used=0;
    for(const auto& c:cases){u32 n=0;while(c.text[n])++n;RoutingMock mock{};
        CHECK(native_font_fallback_routed_range({c.text,n},route_index,font_index,routed_coverage,&mock,&result,100,&used)==SfntStatus::ok);
        CHECK(result.bytes==c.bytes&&result.units==c.units&&result.choice.font==c.font&&!result.choice.policy&&mock.font.probes==c.probes);
        const u32 required=used;for(u32 budget=0;budget<required;++budget){mock={};CHECK(native_font_fallback_routed_range({c.text,n},route_index,font_index,routed_coverage,&mock,&result,budget,&used)==SfntStatus::resource_limit&&empty(result)&&used==budget);}
    }
    RoutingMock mock{};CHECK(native_font_fallback_routed_range({},route_index,font_index,routed_coverage,&mock,&result,0,&used)==SfntStatus::ok&&empty(result)&&!used&&!mock.routes&&!mock.font.selects);
    CHECK(native_font_fallback_routed_range({nullptr,1},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::invalid_input&&empty(result));
    CHECK(native_font_fallback_routed_range({"a",1},nullptr,font_index,routed_coverage,&mock,&result)==SfntStatus::invalid_input&&empty(result));
    CHECK(native_font_fallback_routed_range({"a",1},route_index,nullptr,routed_coverage,&mock,&result)==SfntStatus::invalid_input&&empty(result));
    CHECK(native_font_fallback_routed_range({"a",1},route_index,font_index,nullptr,&mock,&result)==SfntStatus::invalid_input&&empty(result));
    CHECK(native_font_fallback_routed_range({"a",1},route_index,font_index,routed_coverage,&mock,nullptr)==SfntStatus::invalid_input);
    mock.fail_route=true;CHECK(native_font_fallback_routed_range({"a",1},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::truncated&&empty(result));
    mock={};mock.font.fail=1;CHECK(native_font_fallback_routed_range({"a",1},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::malformed&&empty(result));
    mock={};mock.font.fail=2;CHECK(native_font_fallback_routed_range({"a\xcd\x8f",3},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::truncated&&empty(result));
    mock={};mock.font.cover=false;CHECK(native_font_fallback_routed_range({"b\xcd\x8f",3},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::ok&&result.bytes==1&&mock.font.probes==1);
    const char invalid[]{'a',static_cast<char>(0xc0),static_cast<char>(0xaf)};mock={};CHECK(native_font_fallback_routed_range({invalid,3},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::invalid_input&&empty(result));return true;
}
#include "font_fallback_map_cases.inc"
bool guards() noexcept {
    CHECK(routing_guards()&&mapping_guards());
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));
    struct Case {const char* text;u32 bytes,units,font,selects,probes;};
    const Case cases[]{ {"aba",1,1,0,2,0},{"axa",1,1,0,2,0},{"b\xcc\x81" "a",3,2,1,2,1},
        {"b\xe2\x80\x8d" "a",5,3,1,1,2},{"b\xe2\x80\x8c" "a",4,2,1,2,1},
        {"b\xef\xb8\x8f" "a",4,2,1,2,0},{"?\xef\xb8\x8f" "a",1,1,native_fallback_missing,2,0},
        {"b\xe2\x80\x8d\xcc\x81" "a",6,3,1,2,2},{"aaaa",4,4,0,4,0},
        {"b\xf0\x93\x90\xb0" "a",1,1,1,2,0},{"\xf0\x93\x90\xb0" "b",5,3,0,1,1} };
    NativeFallbackRangeResult result{};u32 used=0;
    for(const auto& c:cases){u32 n=0;while(c.text[n])++n;Mock mock{};
        CHECK(native_font_fallback_range({c.text,n},select,coverage,&mock,&result,100,&used)==SfntStatus::ok);
        CHECK(result.bytes==c.bytes&&result.units==c.units&&result.choice.font==c.font&&!result.choice.policy&&mock.selects==c.selects&&mock.probes==c.probes);
        const u32 required=used;for(u32 budget=0;budget<required;++budget){mock={};CHECK(native_font_fallback_range({c.text,n},select,coverage,&mock,&result,budget,&used)==SfntStatus::resource_limit&&empty(result)&&used==budget);}
    }
    Mock mock{};CHECK(native_font_fallback_range({},select,coverage,&mock,&result,0,&used)==SfntStatus::ok&&empty(result)&&!used&&!mock.selects);
    CHECK(native_font_fallback_range({nullptr,1},select,coverage,&mock,&result)==SfntStatus::invalid_input&&empty(result));
    CHECK(native_font_fallback_range({"a",1},nullptr,coverage,&mock,&result)==SfntStatus::invalid_input&&empty(result));
    CHECK(native_font_fallback_range({"a",1},select,nullptr,&mock,&result)==SfntStatus::invalid_input&&empty(result));
    CHECK(native_font_fallback_range({"a",1},select,coverage,&mock,nullptr)==SfntStatus::invalid_input);
    mock.fail=1;CHECK(native_font_fallback_range({"a",1},select,coverage,&mock,&result)==SfntStatus::malformed&&empty(result));
    mock.fail=2;CHECK(native_font_fallback_range({"b\xcc\x81",3},select,coverage,&mock,&result)==SfntStatus::truncated&&empty(result));
    mock={0,0,0,false};CHECK(native_font_fallback_range({"b\xcc\x81",3},select,coverage,&mock,&result)==SfntStatus::ok&&result.bytes==1&&result.choice.font==1&&mock.probes==1);
    const char invalid[]{'a',static_cast<char>(0xc0),static_cast<char>(0xaf)};mock={};CHECK(native_font_fallback_range({invalid,3},select,coverage,&mock,&result)==SfntStatus::invalid_input&&empty(result));
    const u32 invalid_scalars[]{0xd800,0xdfff,0x110000,0xffffffffu};for(u32 cp:invalid_scalars)CHECK(native_font_fallback_class(cp)==native_fallback_invalid);
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup {u8* p;~Cleanup() noexcept{VirtualFree(p,0,MEM_RELEASE);}} cleanup{memory};DWORD old=0;
    char* text=reinterpret_cast<char*>(memory+page-5);copy_bytes(text,"b\xe2\x80\x8d" "a",5);auto* guarded=reinterpret_cast<NativeFallbackRangeResult*>(memory+page*3)-1;
    auto* canary=reinterpret_cast<u32*>(guarded)-1;*canary=0x197ac05d;
    CHECK(VirtualProtect(memory,page,PAGE_READONLY,&old)&&VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)&&VirtualProtect(memory+page*3,page,PAGE_NOACCESS,&old));
    mock={};CHECK(native_font_fallback_range({text,5},select,coverage,&mock,guarded)==SfntStatus::ok&&guarded->bytes==5&&*canary==0x197ac05d);
    RoutingMock routed{};CHECK(native_font_fallback_routed_range({text,5},route_index,font_index,routed_coverage,&routed,guarded)==SfntStatus::ok&&guarded->bytes==5&&*canary==0x197ac05d);
    MapMock mapped{};CHECK(native_font_fallback_map({text,5},map_base,map_route,map_fallback,map_cover,&mapped,guarded)==SfntStatus::ok&&guarded->bytes==4&&*canary==0x197ac05d);
    return true;
}
bool fixture(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);const u8* data=nullptr;
    struct Cleanup {HANDLE file;const u8*& data;~Cleanup() noexcept{if(data)UnmapViewOfFile(data);CloseHandle(file);}} cleanup{file,data};
    LARGE_INTEGER size{};CHECK(GetFileSizeEx(file,&size)&&size.QuadPart==0x110000);HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);CHECK(mapping);
    data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CloseHandle(mapping);CHECK(data);
    for(;classes<0x110000;++classes)CHECK(native_font_fallback_class(classes)==data[classes]);return true;
}
bool argument(const wchar_t** command,wchar_t* out) noexcept {const wchar_t* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){if(n==32767)return false;out[n++]=*p++;}if(quoted){if(*p!='"')return false;++p;}out[n]=0;*command=p;return n!=0;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_fallback_range_entry() noexcept {using namespace maicord;bool ok=guards();const wchar_t* command=GetCommandLineW();wchar_t path[32768];
    if(ok&&argument(&command,path)&&argument(&command,path))ok=fixture(path);print(ok?"PASS native fallback ranges classes=":"FAIL native fallback ranges classes=");number(classes);print(" checks=");number(checks);print("\n");ExitProcess(ok?0:1);}
