#include "maicord/text/fonts/fallback.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "../../support/test_output.hpp"
namespace maicord {namespace {
u32 checks=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);char out[32];for(u32 i=0;i<n;++i)out[i]=s[n-i-1];out[n]=0;print(out);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
bool guards() noexcept {
    const NativeFallbackRange ranges[]{{0,0,3},{100,3,0},{101,1,2},{0x10ffff,0,1}};
    const u16 tokens[]{2,0,1};NativeFallbackCascade c{};
    CHECK(native_fallback_cascade_open({ranges,4},{tokens,3},3,&c)==SfntStatus::ok);
    const u32 points[]{0,99,100,101,0x10fffe,0x10ffff},expected_ranges[]{0,0,1,2,2,3};
    for(u32 i=0;i<6;++i)CHECK(native_fallback_cascade_find(c,points[i])==expected_ranges[i]);
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
    CHECK(native_fallback_cascade_find(c,0)==native_fallback_missing);
    const u16 end=65535;const NativeFallbackRange one{0,0,1};
    CHECK(native_fallback_cascade_open({&one,1},{&end,1},65536,&c)==SfntStatus::ok);
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(pages_allocate(page*4));CHECK(memory);
    struct Cleanup {u8* p;~Cleanup() noexcept{pages_release(p);}} cleanup{memory};CHECK(pages_protect(memory+page, page, PageAccess::none)&&pages_protect(memory+page*3, page, PageAccess::none));
    auto* guarded=reinterpret_cast<NativeFallbackRange*>(memory+page-sizeof(ranges));auto* guarded_tokens=reinterpret_cast<u16*>(memory+page*3-sizeof(tokens));
    copy_bytes(guarded,ranges,sizeof(ranges));copy_bytes(guarded_tokens,tokens,sizeof(tokens));
    CHECK(pages_protect(memory, page, PageAccess::read)&&pages_protect(memory+page*2, page, PageAccess::read));
    CHECK(native_fallback_cascade_open({guarded,4},{guarded_tokens,3},3,&c)==SfntStatus::ok);
    for(u32 scalar=0;scalar<102;++scalar)CHECK(native_fallback_cascade_find(c,scalar)==(scalar<100?0u:scalar-99));
    CHECK(native_fallback_cascade_find(c,0x10ffff)==3);
    return true;
}
}}
MAICORD_TEST_ENTRY(maicord_font_fallback_entry) {
    using namespace maicord;const bool ok=guards();
    print(ok?"PASS native fallback cascade checks=":"FAIL native fallback cascade checks=");number(checks);print("\n");::maicord::test_process_exit(ok?0:1);
}
