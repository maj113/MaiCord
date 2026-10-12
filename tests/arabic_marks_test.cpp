#include "maicord/text/arabic_marks.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {namespace {
#include "generated/arabic_marks_fixture.inc"
bool corpus() noexcept {
    constexpr u32 size=sizeof(arabic_mark_vectors)/sizeof(u32);u32 at=0,cases=0,order[64];
    while(at<size) {
        if(size-at<2)return false;const u32 n=arabic_mark_vectors[at++],expected=arabic_mark_vectors[at++];
        if(n>64 || n+expected>size-at)return false;const auto* input=arabic_mark_vectors+at;const auto* want=input+n;
        if(sfnt_arabic_mark_order({input,n},{order,64},n*16)!=SfntStatus::ok)return false;
        u64 seen=0;u32 checked=0;
        for(u32 i=0;i<n;++i){if(order[i]>=n || (seen&(1ull<<order[i])))return false;seen|=1ull<<order[i];const auto scalar=input[order[i]];
            if(scalar==0x34f)continue;if(checked==expected || scalar!=want[checked++])return false;}
        if(checked!=expected)return false;at+=n+expected;++cases;
    }
    return cases==arabic_mark_cases;
}
bool contracts() noexcept {
    u32 output[8];for(auto& v:output)v=0xeeeeeeee;const u32 input[]{0x628,0x64e,0x651,0x628},invalid[]{0x628,0xd800};
    if(sfnt_arabic_mark_order({input,4},{output,3})!=SfntStatus::output_too_small ||
       sfnt_arabic_mark_order({input,4},{output,8},63)!=SfntStatus::resource_limit ||
       sfnt_arabic_mark_order({invalid,2},{output,8})!=SfntStatus::invalid_input ||
       sfnt_arabic_mark_order({nullptr,1},{output,8})!=SfntStatus::invalid_input ||
       sfnt_arabic_mark_order({input,4},{nullptr,8})!=SfntStatus::invalid_input ||
       sfnt_arabic_mark_order({},{},0)!=SfntStatus::ok)return false;
    for(auto v:output)if(v!=0xeeeeeeee)return false;
    u32 overlap[]{0x628,0x64e,0x651,0x628};if(sfnt_arabic_mark_order({overlap,4},{overlap,4})!=SfntStatus::invalid_input)return false;
    if(!bytes_equal(overlap,input,sizeof(input)))return false;
    if(sfnt_arabic_mark_order({input,4},{output,8},64)!=SfntStatus::ok || output[0]!=0 || output[1]!=2 || output[2]!=1 || output[3]!=3)return false;
    for(u32 i=4;i<8;++i)if(output[i]!=0xeeeeeeee)return false;return true;
}
bool long_span() noexcept {
    constexpr u32 n=12288;auto* input=static_cast<u32*>(VirtualAlloc(nullptr,n*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* output=static_cast<u32*>(VirtualAlloc(nullptr,n*4+4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!input || !output){if(input)VirtualFree(input,0,MEM_RELEASE);if(output)VirtualFree(output,0,MEM_RELEASE);return false;}
    for(u32 i=0;i<n;++i)input[i]=i%3==0?0x64e:i%3==1?0x651:0x655;
    DWORD old=0;bool ok=VirtualProtect(input,n*4,PAGE_READONLY,&old)!=0 && VirtualProtect(output+n,4096,PAGE_NOACCESS,&old)!=0;
    ok=ok && sfnt_arabic_mark_order({input,n},{output,n},n*16)==SfntStatus::ok;
    for(u32 i=0;i<n && ok;++i){const u32 group=i/(n/3),rank=i%(n/3);ok=output[i]==rank*3+(group==0?2:group==1?1:0);}
    VirtualFree(output,0,MEM_RELEASE);VirtualFree(input,0,MEM_RELEASE);return ok;
}
int run() noexcept {
    if(!corpus())return 1;if(!contracts())return 2;if(!long_span())return 3;
    constexpr auto message=utf8_literal("PASS native Arabic mark order: 2873 Windows cases, stable buckets, 12288-mark guarded span\n");DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message.data,message.bytes,&done,nullptr);return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_arabic_marks_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
