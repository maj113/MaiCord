#include "maicord/text/font_fallback_catalog.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
u32 checks=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char s[32],out[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n;++i)out[i]=s[n-i-1];out[n]=0;print(out);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
bool memory_guards() noexcept {
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;CHECK(page>=4096);
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,static_cast<SIZE_T>(page)*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup {u8* memory;~Cleanup() noexcept{VirtualFree(memory,0,MEM_RELEASE);}} cleanup{memory};
    auto* input=reinterpret_cast<NativeFontProperties*>(memory+page)-4;const NativeFontProperties source[]{{400,5,0},{300,5,0},{700,5,0},{400,5,2}};copy_bytes(input,source,sizeof(source));
    auto* output=reinterpret_cast<u32*>(memory+page*3)-4;output[-1]=0x197ac05d;DWORD old=0;
    CHECK(VirtualProtect(memory,page,PAGE_READONLY,&old)&&VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)&&VirtualProtect(memory+page*3,page,PAGE_NOACCESS,&old));
    u32 count=99,used=0;CHECK(native_font_fallback_order({input,4},{600,5,0},{output,4},&count,100,&used)==SfntStatus::ok&&count==4);
    const u32 expected[]{2,1,0,3};CHECK(bytes_equal(output,expected,sizeof(expected))&&output[-1]==0x197ac05d&&bytes_equal(input,source,sizeof(source)));
    CHECK(native_font_fallback_order({input,4},{600,5,0},{output,4},&count,used-1,&used)==SfntStatus::resource_limit&&!count&&output[-1]==0x197ac05d);return true;
}
bool guards() noexcept {
    CHECK(memory_guards());
    const NativeFontProperties input[]{{400,5,0},{300,5,0},{700,5,0},{400,5,2}};u32 output[4]{},count=99,used=0;
    CHECK(native_font_fallback_order({input,4},{600,5,0},{output,4},&count,100,&used)==SfntStatus::ok&&count==4);
    const u32 expected[]{2,1,0,3};CHECK(bytes_equal(output,expected,sizeof(expected)));const u32 needed=used;
    for(u32 budget=0;budget<needed;++budget)CHECK(native_font_fallback_order({input,4},{600,5,0},{output,4},&count,budget,&used)==SfntStatus::resource_limit&&!count&&used==budget);
    CHECK(native_font_fallback_order({input,4},{400,5,0},{output,3},&count)==SfntStatus::output_too_small&&!count);
    CHECK(native_font_fallback_order({nullptr,4},{400,5,0},{output,4},&count)==SfntStatus::invalid_input&&!count);
    CHECK(native_font_fallback_order({input,4},{400,5,0},{nullptr,4},&count)==SfntStatus::invalid_input&&!count);
    CHECK(native_font_fallback_order({input,4},{400,5,0},{output,4},nullptr)==SfntStatus::invalid_input);
    CHECK(native_font_fallback_order({},{400,5,0},{},&count,0,&used)==SfntStatus::unsupported_sequence&&!count&&!used);
    const NativeFontProperties invalid[]{{0,5,0},{1001,5,0},{400,0,0},{400,10,0},{400,5,3}};
    for(auto p:invalid){CHECK(native_font_fallback_order({&p,1},{400,5,0},{output,4},&count)==SfntStatus::invalid_input&&!count);CHECK(native_font_fallback_order({input,4},p,{output,4},&count)==SfntStatus::invalid_input&&!count);}
    NativeFontCatalogCandidate candidates[4]{};NativeFontCatalogFace faces[4]{};u16 names[]{'P','r','o','b','e',0};
    for(u32 i=0;i<4;++i){candidates[i]={input[i],i,0xffffffffu,0,0};faces[i]={0,i,0,5,i,1};}
    NativeFontCatalog catalog{{},{faces,4},{candidates,4},{},{names,6},{0,4,4,0,6}};NativeFontProperties scratch[4];u32 indices[4];
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count)==SfntStatus::ok&&count==4&&bytes_equal(output,expected,sizeof(expected)));
    const u16 lower[]{'p','r','o','b','e'};
    CHECK(native_font_catalog_fallback_order(catalog,{lower,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count,100,100,&used)==SfntStatus::ok&&count==4&&bytes_equal(output,expected,sizeof(expected)));
    const u32 catalog_work=used;for(u32 budget=0;budget<catalog_work;++budget)CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count,100,budget,&used)==SfntStatus::resource_limit&&!count&&used==budget);
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count,100,catalog_work,&used)==SfntStatus::ok&&count==4&&used==catalog_work);
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count,3)==SfntStatus::resource_limit&&!count);
    const u16 missing[]{'N','o'};CHECK(native_font_catalog_fallback_order(catalog,{missing,2},{600,5,0},{scratch,4},{indices,4},{output,4},&count)==SfntStatus::unsupported_sequence&&!count);
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,3},{indices,4},{output,4},&count)==SfntStatus::output_too_small&&!count);
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,3},{output,4},&count)==SfntStatus::output_too_small&&!count);
    const auto saved=faces[3];faces[3].first_candidate=4;CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count)==SfntStatus::invalid_input&&!count);faces[3]=saved;
    candidates[3].face=0;CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count)==SfntStatus::invalid_input&&!count);candidates[3].face=3;
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_fallback_family_entry() noexcept {using namespace maicord;const bool ok=guards();
    print(ok?"PASS native fallback families checks=":"FAIL native fallback families checks=");number(checks);print("\n");ExitProcess(ok?0:1);}
