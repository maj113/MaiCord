#include "maicord/text/font_fallback.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
u32 checks=0;u64 probes=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);char out[32];for(u32 i=0;i<n;++i)out[i]=s[n-i-1];out[n]=0;print(out);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
struct Mock {u32 available=0,called=0,last=0,fail=native_fallback_missing;};
SfntStatus covered(void* context,u32 token,u32 scalar,bool* out) noexcept {
    auto& m=*static_cast<Mock*>(context);++m.called;m.last=scalar;*out=false;
    if(token==m.fail)return SfntStatus::malformed;*out=(m.available&(1u<<token))!=0;return SfntStatus::ok;
}
bool guards() noexcept {
    const NativeFallbackRange ranges[]{{0,0,3},{100,3,0},{101,1,2},{0x10ffff,0,1}};
    const u16 tokens[]{2,0,1};NativeFallbackCascade c{};Mock mock{};u32 token=0,used=99;
    CHECK(native_fallback_cascade_open({ranges,4},{tokens,3},3,&c)==SfntStatus::ok);
    const u32 points[]{0,99,100,101,0x10fffe,0x10ffff},expected_ranges[]{0,0,1,2,2,3};
    for(u32 i=0;i<6;++i){CHECK(native_fallback_cascade_find(c,points[i],&token,100,&used)==SfntStatus::ok&&token==expected_ranges[i]);
        const u32 needed=used;for(u32 limit=0;limit<needed;++limit)CHECK(native_fallback_cascade_find(c,points[i],&token,limit,&used)==SfntStatus::resource_limit&&used==limit&&token==native_fallback_missing);
        CHECK(native_fallback_cascade_find(c,points[i],&token,needed,&used)==SfntStatus::ok&&token==expected_ranges[i]&&used==needed);}
    CHECK(native_fallback_cascade_find(c,0,nullptr)==SfntStatus::invalid_input);
    for(u32 mask=0;mask<8;++mask){mock={mask};CHECK(native_fallback_cascade_select(c,99,covered,&mock,&token)==SfntStatus::ok);
        CHECK(token==((mask&4)?2u:(mask&1)?0u:(mask&2)?1u:native_fallback_missing));}
    mock={7};CHECK(native_fallback_cascade_select(c,100,covered,&mock,&token,100,&used)==SfntStatus::ok&&token==native_fallback_missing&&!mock.called&&used);
    CHECK(native_fallback_cascade_select(c,101,covered,&mock,&token,100,&used)==SfntStatus::ok&&token==0&&mock.last==101);
    CHECK(native_fallback_cascade_select(c,0x10ffff,covered,&mock,&token)==SfntStatus::ok&&token==2);
    const u32 invalid[]{0xd800,0xdfff,0x110000,0xffffffffu};
    for(u32 scalar:invalid){mock={};CHECK(native_fallback_cascade_select(c,scalar,covered,&mock,&token,100,&used)==SfntStatus::invalid_input&&token==native_fallback_missing&&!used&&!mock.called);
        CHECK(native_fallback_cascade_find(c,scalar,&token,100,&used)==SfntStatus::invalid_input&&token==native_fallback_missing&&!used);}
    CHECK(native_fallback_cascade_select(c,0,nullptr,nullptr,&token)==SfntStatus::invalid_input&&token==native_fallback_missing);
    CHECK(native_fallback_cascade_select(c,0,covered,&mock,nullptr)==SfntStatus::invalid_input);
    mock={1};CHECK(native_fallback_cascade_select(c,0,covered,&mock,&token,100,&used)==SfntStatus::ok&&token==0&&mock.called==2);
    const u32 required=used;
    for(u32 budget=0;budget<required;++budget){mock={1};CHECK(native_fallback_cascade_select(c,0,covered,&mock,&token,budget,&used)==SfntStatus::resource_limit&&token==native_fallback_missing&&used==budget);}
    mock={1,0,0,2};CHECK(native_fallback_cascade_select(c,0,covered,&mock,&token)==SfntStatus::malformed&&token==native_fallback_missing&&mock.called==1);
    CHECK(native_fallback_cascade_open({ranges,4},{tokens,3},3,&c,6)==SfntStatus::resource_limit&&!c.ranges.count);
    CHECK(native_fallback_cascade_open({nullptr,1},{},3,&c)==SfntStatus::invalid_input);
    CHECK(native_fallback_cascade_open({},{nullptr,1},3,&c)==SfntStatus::invalid_input);
    CHECK(native_fallback_cascade_open({ranges,4},{tokens,3},2,&c)==SfntStatus::invalid_input);
    CHECK(native_fallback_cascade_open({},{},65537,&c)==SfntStatus::invalid_input);
    CHECK(native_fallback_cascade_open({},{},0,nullptr)==SfntStatus::invalid_input);
    const NativeFallbackRange bad[]{{1,0,0},{0,4,0},{0,2,2},{0x110000,0,0}};
    for(auto r:bad)CHECK(native_fallback_cascade_open({&r,1},{tokens,3},3,&c)==SfntStatus::invalid_input&&!c.ranges.count);
    const NativeFallbackRange reversed[]{{0,0,1},{3,0,0},{2,0,0}},duplicate[]{{0,0,1},{0,0,0}};
    CHECK(native_fallback_cascade_open({reversed,3},{tokens,3},3,&c)==SfntStatus::invalid_input);
    CHECK(native_fallback_cascade_open({duplicate,2},{tokens,3},3,&c)==SfntStatus::invalid_input);
    CHECK(native_fallback_cascade_open({},{},0,&c,0)==SfntStatus::ok);
    CHECK(native_fallback_cascade_find(c,0,&token,0,&used)==SfntStatus::ok&&token==native_fallback_missing&&!used);
    mock={7};CHECK(native_fallback_cascade_select(c,0,covered,&mock,&token,0,&used)==SfntStatus::ok&&token==native_fallback_missing&&!mock.called&&!used);
    const u16 end=65535;const NativeFallbackRange one{0,0,1};
    CHECK(native_fallback_cascade_open({&one,1},{&end,1},65536,&c)==SfntStatus::ok);
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup {u8* p;~Cleanup() noexcept{VirtualFree(p,0,MEM_RELEASE);}} cleanup{memory};DWORD old=0;
    CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)&&VirtualProtect(memory+page*3,page,PAGE_NOACCESS,&old));
    auto* guarded=reinterpret_cast<NativeFallbackRange*>(memory+page-sizeof(ranges));auto* guarded_tokens=reinterpret_cast<u16*>(memory+page*3-sizeof(tokens));
    copy_bytes(guarded,ranges,sizeof(ranges));copy_bytes(guarded_tokens,tokens,sizeof(tokens));
    CHECK(VirtualProtect(memory,page,PAGE_READONLY,&old)&&VirtualProtect(memory+page*2,page,PAGE_READONLY,&old));
    CHECK(native_fallback_cascade_open({guarded,4},{guarded_tokens,3},3,&c)==SfntStatus::ok);
    for(u32 scalar=0;scalar<102;++scalar){mock={3};CHECK(native_fallback_cascade_select(c,scalar,covered,&mock,&token)==SfntStatus::ok&&token==(scalar==100?native_fallback_missing:0));}
    CHECK(native_fallback_cascade_find(c,0x10ffff,&token)==SfntStatus::ok&&token==3);
    return true;
}
struct File {
    HANDLE file=INVALID_HANDLE_VALUE;const u8* data=nullptr;u32 size=0;
    bool open(const wchar_t* path) noexcept {file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
        LARGE_INTEGER n{};if(!GetFileSizeEx(file,&n)||n.QuadPart<=0||n.QuadPart>0xffffffffll)return false;size=static_cast<u32>(n.QuadPart);
        HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping)return false;data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CloseHandle(mapping);return data!=nullptr;}
    ~File() noexcept {if(data)UnmapViewOfFile(data);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
};
struct Fonts {File files[64];SfntCmap cmaps[64];u32 count=0;};
SfntStatus font_coverage(void* context,u32 token,u32 scalar,bool* out) noexcept {
    auto& fonts=*static_cast<Fonts*>(context);*out=false;++probes;if(token>=fonts.count)return SfntStatus::invalid_input;
    u16 glyph=0;const auto status=sfnt_cmap_glyph(fonts.cmaps[token],scalar,0,&glyph);if(status==SfntStatus::ok)*out=glyph!=0;return status;
}
bool fixture(const wchar_t* path) noexcept {
    File file;CHECK(file.open(path)&&file.size>=20);const auto* p=file.data;
    CHECK(byte_load_le32(p)==0x3143464d);const u32 nr=byte_load_le32(p+4),nt=byte_load_le32(p+8),nf=byte_load_le32(p+12),nc=byte_load_le32(p+16);
    CHECK(nr<=1024&&nt<=8192&&nf<=64&&nc==0x110000);u32 at=20;NativeFallbackRange ranges[1024];u16 tokens[8192];Fonts fonts;fonts.count=nf;
    CHECK(nr*8<=file.size-at);copy_bytes(ranges,p+at,nr*8);at+=nr*8;CHECK(nt*2<=file.size-at);copy_bytes(tokens,p+at,nt*2);at+=nt*2;
    for(u32 i=0;i<nf;++i){CHECK(file.size-at>=8);const u32 units=byte_load_le32(p+at),index=byte_load_le32(p+at+4);at+=8;
        CHECK(units<32768&&units*2<=file.size-at);wchar_t font_path[32768];copy_bytes(font_path,p+at,units*2);font_path[units]=0;at+=units*2;
        CHECK(fonts.files[i].open(font_path));SfntFace face{};CHECK(sfnt_open({fonts.files[i].data,fonts.files[i].size},index,&face)==SfntStatus::ok);
        CHECK(sfnt_cmap_open(face,fonts.cmaps+i)==SfntStatus::ok);}
    CHECK(file.size-at==nc*4);NativeFallbackCascade cascade{};CHECK(native_fallback_cascade_open({ranges,nr},{tokens,nt},nf,&cascade)==SfntStatus::ok);
    LARGE_INTEGER start{},end{},frequency{};CHECK(QueryPerformanceFrequency(&frequency)&&QueryPerformanceCounter(&start));
    for(u32 cp=0;cp<nc;++cp){u32 token=0;const auto status=native_fallback_cascade_select(cascade,cp,font_coverage,&fonts,&token);
        if(cp>=0xd800&&cp<=0xdfff){CHECK(status==SfntStatus::invalid_input&&token==native_fallback_missing);continue;}
        const u32 expected=byte_load_le32(p+at+cp*4);
        if(status!=SfntStatus::ok||token!=expected){print("scalar=");number(cp);print(" status=");number(static_cast<u32>(status));print(" actual=");number(token);print(" expected=");number(expected);print("\n");}
        CHECK(status==SfntStatus::ok&&token==expected);}
    CHECK(QueryPerformanceCounter(&end));print("observations=");number(nc-2048);print(" probes=");number(probes);print(" ticks=");number(end.QuadPart-start.QuadPart);print(" frequency=");number(frequency.QuadPart);print("\n");return true;
}
bool argument(const wchar_t** command,wchar_t* out) noexcept {
    const wchar_t* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){if(n==32767)return false;out[n++]=*p++;}if(quoted){if(*p!='"')return false;++p;}out[n]=0;*command=p;return n!=0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_fallback_entry() noexcept {
    using namespace maicord;bool ok=guards();const wchar_t* command=GetCommandLineW();wchar_t path[32768];
    if(ok&&argument(&command,path)&&argument(&command,path))ok=fixture(path);
    print(ok?"PASS native fallback cascade checks=":"FAIL native fallback cascade checks=");number(checks);print("\n");ExitProcess(ok?0:1);
}
