#include "maicord/text/font_match.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
#include "generated/font_matching_fixture.inc"
u32 checks=0,cases=0,groups=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&written,nullptr);}
void number(u32 value) noexcept {char s[16];u32 n=0;do{s[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}s[n]=0;print(s);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print(" case=");number(cases);print(" group=");number(groups);print("\n");return false;}}while(0)
bool corpus() noexcept {
    const auto* data=font_matching_fixture;const u32 size=sizeof(font_matching_fixture);u32 at=12;CHECK(bytes_equal(data,"MCFMAT1\0",8));const u32 n=byte_load_le32(data+8);
    for(u32 g=0;g<n;++g){CHECK(size-at>=8);const u32 count=byte_load_le32(data+at),queries=byte_load_le32(data+at+4);at+=8;CHECK(count&&count<=256&&size-at>=count*4);
        NativeFontProperties candidates[256];copy_bytes(candidates,data+at,count*4);at+=count*4;
        for(u32 q=0;q<queries;++q){CHECK(size-at>=16);NativeFontProperties request{},expected{};copy_bytes(&request,data+at,4);const u32 index=byte_load_le32(data+at+4),simulations=byte_load_le32(data+at+8);copy_bytes(&expected,data+at+12,4);at+=16;
            NativeFontMatch actual{};CHECK(native_font_match({candidates,count},request,&actual)==SfntStatus::ok);
            if(actual.index!=index||actual.simulations!=simulations||!bytes_equal(&actual.properties,&expected,4)){print("request=");number(request.weight);print(",");number(request.width);print(",");number(request.style);print(" actual=");number(actual.index);print(",");number(actual.simulations);print(" expected=");number(index);print(",");number(simulations);print("\n");}
            CHECK(actual.index==index&&actual.simulations==simulations&&bytes_equal(&actual.properties,&expected,4));++cases;
        }++groups;
    }CHECK(at==size);return true;
}
bool guards() noexcept {
    NativeFontMatch result{};const NativeFontProperties normal{400,5,0};
    CHECK(native_font_match({},normal,&result)==SfntStatus::unsupported_sequence&&!result.index&&!result.properties.weight&&!result.simulations);
    CHECK(native_font_match({nullptr,1},normal,&result)==SfntStatus::invalid_input);
    CHECK(native_font_match({&normal,1},normal,nullptr)==SfntStatus::invalid_input);
    CHECK(native_font_match({&normal,1},normal,&result,0)==SfntStatus::resource_limit&&!result.properties.weight);
    const NativeFontProperties invalid[]{{0,5,0},{1001,5,0},{400,0,0},{400,10,0},{400,5,3}};
    for(auto bad:invalid){CHECK(native_font_match({&normal,1},bad,&result)==SfntStatus::invalid_input);const NativeFontProperties list[]{normal,bad};CHECK(native_font_match({list,2},normal,&result)==SfntStatus::invalid_input&&!result.properties.weight);}
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup{u8* p;~Cleanup() noexcept{VirtualFree(p,0,MEM_RELEASE);}} cleanup{memory};DWORD old=0;
    CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)&&VirtualProtect(memory+page*3,page,PAGE_NOACCESS,&old));
    auto* input=reinterpret_cast<NativeFontProperties*>(memory+page-12);input[0]=normal;input[1]={700,5,0};input[2]=normal;
    auto* output=reinterpret_cast<NativeFontMatch*>(memory+page*3-sizeof(NativeFontMatch));
    CHECK(VirtualProtect(memory,page,PAGE_READONLY,&old));
    CHECK(native_font_match({input,3},normal,output)==SfntStatus::ok&&output->index==0&&!output->simulations);
    CHECK(native_font_match({input,3},{700,5,0},output)==SfntStatus::ok&&output->index==1&&!output->simulations);
    CHECK(native_font_match({input,1},{700,5,2},output)==SfntStatus::ok&&output->index==0&&output->simulations==3&&output->properties.weight==700&&output->properties.style==1);
    const NativeFontProperties extremes[]{{1,1,0},{1000,9,2}};
    for(auto request:extremes)for(auto candidate:extremes)CHECK(native_font_match({&candidate,1},request,output)==SfntStatus::ok);
    return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_match_entry() noexcept {
    using namespace maicord;const bool success=corpus()&&guards();print("groups=");number(groups);print(" cases=");number(cases);print(" checks=");number(checks);print(success?"\nPASS native font matching\n":"\nFAIL native font matching\n");ExitProcess(success?0:1);
}
