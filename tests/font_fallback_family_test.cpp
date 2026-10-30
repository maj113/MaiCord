#include "maicord/text/font_match.hpp"
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
    native_font_fallback_order({input,4},{600,5,0},output);
    const u32 expected[]{2,1,0,3};CHECK(bytes_equal(output,expected,sizeof(expected))&&output[-1]==0x197ac05d&&bytes_equal(input,source,sizeof(source)));return true;
}
bool guards() noexcept {
    CHECK(memory_guards());
    u32 output[4]{};const NativeFontProperties normal[]{{400,5,0},{400,5,0},{400,5,0}};
    // Equal candidates keep source order.
    native_font_fallback_order({normal,3},{400,5,0},output);CHECK(output[0]==0&&output[1]==1&&output[2]==2);
    // Simulation flags break an equal projection: bold simulated before oblique simulated.
    const NativeFontProperties ties[]{{400,5,1},{700,5,0}};native_font_fallback_order({ties,2},{700,5,1},output);CHECK(output[0]==0&&output[1]==1);
    const NativeFontProperties reversed[]{{700,5,0},{400,5,1}};native_font_fallback_order({reversed,2},{700,5,1},output);CHECK(output[0]==1&&output[1]==0);
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_fallback_family_entry() noexcept {using namespace maicord;const bool ok=guards();
    print(ok?"PASS native fallback families checks=":"FAIL native fallback families checks=");number(checks);print("\n");ExitProcess(ok?0:1);}
