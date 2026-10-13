// No argument runs the registered checks. Manual: MAICORD_KHMER_INPUT_STREAM=1|2 answers cluster
// queries from stdin for the Python oracles (QA only).
#include "maicord/text/khmer_input.hpp"
#include "../src/text/khmer_character.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
u32 checks=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char b[32];u32 n=0;do{b[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n){const char s[]{b[--n],0};print(s);}}
bool fail(u32 line) noexcept {print("FAIL Khmer input line ");number(line);print("\n");return false;}
#define CHECK(x) do{++checks;if(!(x))return fail(__LINE__);}while(false)
bool boundaries(ConstSlice<u32> input,ConstSlice<u8> expected) noexcept {
    u8 output[256];CHECK(input.count==expected.count&&input.count<=256);
    for(u64 budget=0;budget<=input.count*4u;++budget){for(auto& c:output)c=255;
        const auto status=sfnt_khmer_cluster_boundaries(input,{output,256},budget);
        CHECK(status==(budget<input.count*4u?SfntStatus::resource_limit:SfntStatus::ok));
        if(status==SfntStatus::ok)CHECK(bytes_equal(output,expected.data,expected.count));
        else for(u8 c:output)CHECK(c==255);
    }return true;
}
bool units() noexcept {
    const u32 a[]{0x1780,0x17d2,0x179a,0x17d2,0x179a};const u8 ae[]{1,0,0,1,0};
    const u32 b[]{0x1780,0x17be,0x17b6,0x17c6,0x200d};const u8 be[]{1,0,0,1,0};
    const u32 c[]{0x17d2,0xfe00,0xfe00,0x1780,0x17cc,0x17d2,0x1780};const u8 ce[]{1,0,0,1,0,0,0};
    CHECK(boundaries({a,5},{ae,5}));CHECK(boundaries({b,5},{be,5}));CHECK(boundaries({c,7},{ce,7}));
    const u32 selectors[]{0x1780,0x17d2,0x179a,0x17d2,0xe0100,0x179a,0x17c1};const u8 se[]{1,0,0,1,0,0,1};
    CHECK(boundaries({selectors,7},{se,7}));
    const u32 shifts[]{0x1780,0x17c9,0x17d2,0x1780,0x17c9,0x17d2,0x1780,0x17c1};const u8 she[]{1,0,0,0,0,0,0,1};
    CHECK(boundaries({shifts,8},{she,8}));
    u32 long_input[256];u8 expected[256],output[256];long_input[0]=0x1780;expected[0]=1;
    for(u32 i=1;i<256;++i){long_input[i]=0x17c6;expected[i]=0;}
    CHECK(boundaries({long_input,256},{expected,256}));
    for(auto& x:output)x=255;
    CHECK(sfnt_khmer_cluster_boundaries({a,5},{output,4})==SfntStatus::output_too_small);
    for(u8 x:output)CHECK(x==255);
    const u32 invalid[]{0x1780,0xd800};
    CHECK(sfnt_khmer_cluster_boundaries({invalid,2},{output,64})==SfntStatus::invalid_input);
    for(u8 x:output)CHECK(x==255);
    CHECK(sfnt_khmer_cluster_boundaries({nullptr,1},{output,64})==SfntStatus::invalid_input);
    CHECK(sfnt_khmer_cluster_boundaries({a,5},{nullptr,5})==SfntStatus::invalid_input);
    CHECK(sfnt_khmer_cluster_boundaries({long_input,4},{reinterpret_cast<u8*>(long_input),4})==SfntStatus::invalid_input);
    CHECK(sfnt_khmer_cluster_boundaries({}, {},0)==SfntStatus::ok);
    const u32 original[]{0x1f600,0x1780,0x17be,0x1780,0x17bf,0x1780,0x17c0,0x1780,0x17c4,0x1780,0x17c5};
    const SfntKhmerInputScalar expanded[]{{0x1f600,0},{0x1780,2},{0x17c1,3},{0x17be,3},{0x1780,4},{0x17c1,5},{0x17bf,5},
        {0x1780,6},{0x17c1,7},{0x17c0,7},{0x1780,8},{0x17c1,9},{0x17c4,9},{0x1780,10},{0x17c1,11},{0x17c5,11}};
    SfntKhmerInputScalar actual[16];SfntKhmerInputResult result{};
    for(u64 budget=0;budget<=33;++budget){for(auto& v:actual)v={0xffffffffu,0xffffffffu};
        const auto status=sfnt_khmer_expand_cluster({original,11},{actual,16},&result,budget);
        CHECK(status==(budget<33?SfntStatus::resource_limit:SfntStatus::ok));
        if(status==SfntStatus::ok)CHECK(result.scalar_count==16&&result.source_units==12&&bytes_equal(actual,expanded,sizeof(expanded)));
        else {CHECK(!result.scalar_count&&!result.source_units);for(const auto& v:actual)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);}
    }
    for(auto& v:actual)v={0xffffffffu,0xffffffffu};
    CHECK(sfnt_khmer_expand_cluster({original,11},{actual,15},&result)==SfntStatus::output_too_small&&result.scalar_count==16&&result.source_units==12);
    for(const auto& v:actual)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);
    CHECK(sfnt_khmer_expand_cluster({invalid,2},{actual,16},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    for(const auto& v:actual)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);
    CHECK(sfnt_khmer_expand_cluster({reinterpret_cast<u32*>(actual),2},{actual,16},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_khmer_expand_cluster({}, {},&result,0)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    CHECK(sfnt_khmer_expand_cluster({}, {},nullptr)==SfntStatus::invalid_input);
    const u32 compound[]{0x1780,0x17be,0x17c6,0x17c4};
    const SfntKhmerInputScalar one_split[]{{0x1780,0},{0x17c1,1},{0x17be,1},{0x17c6,2},{0x17c4,3}};
    CHECK(sfnt_khmer_expand_cluster({compound,4},{actual,16},&result)==SfntStatus::ok&&result.scalar_count==5&&bytes_equal(actual,one_split,sizeof(one_split)));
    const u32 opaque[]{0x17d2,0x17be,0x1780,0x17b7,0x17c4};
    CHECK(sfnt_khmer_expand_cluster({opaque,5},{actual,16},&result)==SfntStatus::ok&&result.scalar_count==5);
    for(u32 i=0;i<5;++i)CHECK(actual[i].scalar==opaque[i]&&actual[i].source==i);
    SYSTEM_INFO system{};GetSystemInfo(&system);const u32 page=system.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE,PAGE_NOACCESS));CHECK(memory);
    const bool committed=VirtualAlloc(memory,page,MEM_COMMIT,PAGE_READWRITE)&&VirtualAlloc(memory+page*2,page,MEM_COMMIT,PAGE_READWRITE);
    bool good=false;
    if(committed){auto* input=reinterpret_cast<u32*>(memory+page-sizeof(original));copy_bytes(input,original,sizeof(original));
        auto* target=reinterpret_cast<SfntKhmerInputScalar*>(memory+page*3-sizeof(expanded));
        good=sfnt_khmer_expand_cluster({input,11},{target,16},&result)==SfntStatus::ok&&bytes_equal(target,expanded,sizeof(expanded));
        good=good&&sfnt_khmer_cluster_boundaries({input,11},{memory+page*3-11,11})==SfntStatus::ok;
    }
    CHECK(VirtualFree(memory,0,MEM_RELEASE));CHECK(committed&&good);
    print("PASS Khmer input; assertions=");number(checks);print("\n");return true;
}
bool read(void* destination,u32 count) noexcept {u8* out=static_cast<u8*>(destination);while(count){DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),out,count,&got,nullptr)||!got)return false;out+=got;count-=got;}return true;}
bool write(const void* source,u32 count) noexcept {const u8* in=static_cast<const u8*>(source);while(count){DWORD done=0;if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),in,count,&done,nullptr)||!done)return false;in+=done;count-=done;}return true;}
bool stream(bool expand) noexcept {
    u8 header[12];CHECK(read(header,12)&&bytes_equal(header,"MDVCL1\0\0",8));const u32 cases=byte_load_le32(header+8);CHECK(cases&&cases<=100000&&write(header,12));
    for(u32 row=0;row<cases;++row){u32 n=0,input[256];u8 boundaries[256];SfntKhmerInputScalar output[512];SfntKhmerInputResult result{};
        CHECK(read(&n,4)&&n&&n<=256&&read(input,n*4));
        if(expand){CHECK(sfnt_khmer_expand_cluster({input,n},{output,512},&result)==SfntStatus::ok&&write(&result,sizeof(result))&&write(output,result.scalar_count*sizeof(output[0])));}
        else CHECK(sfnt_khmer_cluster_boundaries({input,n},{boundaries,256})==SfntStatus::ok&&write(boundaries,n));
    }return true;
}
int run() noexcept {
    wchar_t mode[16];const DWORD n=GetEnvironmentVariableW(L"MAICORD_KHMER_INPUT_STREAM",mode,16);
    if(n)return n<16&&stream(mode[0]=='2')?0:2;
    return units()?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_khmer_input_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
