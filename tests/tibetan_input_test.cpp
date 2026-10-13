// No argument runs the registered checks. Manual: MAICORD_TIBETAN_INPUT_STREAM=1|2 answers cluster
// queries from stdin for the Python oracles (QA only).
#include "maicord/text/tibetan_input.hpp"
#include "../src/text/tibetan_character.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
u32 checks=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char b[32];u32 n=0;do{b[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n){const char s[]{b[--n],0};print(s);}}
bool fail(u32 line) noexcept {print("FAIL Tibetan input line ");number(line);print("\n");return false;}
#define CHECK(x) do{++checks;if(!(x))return fail(__LINE__);}while(false)
bool boundaries(ConstSlice<u32> input,ConstSlice<u8> expected) noexcept {
    u8 output[256];CHECK(input.count==expected.count&&input.count<=256);
    for(u64 budget=0;budget<=input.count*4u;++budget){for(auto& c:output)c=255;
        const auto status=sfnt_tibetan_cluster_boundaries(input,{output,256},budget);
        CHECK(status==(budget<input.count*4u?SfntStatus::resource_limit:SfntStatus::ok));
        if(status==SfntStatus::ok)CHECK(bytes_equal(output,expected.data,expected.count));
        else for(u8 c:output)CHECK(c==255);
    }return true;
}
bool units() noexcept {
    const u32 a[]{0xf40,0xf39,0xf71,0xf90,0xf72,0xf35,0xf35};const u8 ae[]{1,0,0,0,0,0,1};CHECK(boundaries({a,7},{ae,7}));
    const u32 b[]{0xf40,0xf72,0xf74,0xf72};const u8 be[]{1,0,1,1};CHECK(boundaries({b,4},{be,4}));
    const u32 c[]{0xf40,0xf77,0xf77,0xf3e,0xf7e,0xf35,0xf72};CHECK(boundaries({c,7},{ae,7}));
    const u32 join[]{0xf40,0xfe00,0x34f,0x200c,0xfe00,0xf72,0xf40};const u8 je[]{1,0,0,0,0,1,1};CHECK(boundaries({join,7},{je,7}));
    const u32 selectors[]{0xf40,0xe0100,0xe0100,0x200d,0xfe00,0x34f};const u8 se[]{1,0,1,1,0,1};CHECK(boundaries({selectors,6},{se,6}));
    const u32 placeholders[]{0xa0,0xd7,0x2d,0x2012,0x2013,0x2014,0x2015,0x2022,0x25cc,0x25fb,0x25fc,0x25fd,0x25fe,0xf20,0xf85};
    for(u32 scalar:placeholders){const u32 text[]{scalar,0xf90,0xf72};const u8 expected[]{1,0,0};CHECK(boundaries({text,3},{expected,3}));}
    u32 long_input[64];u8 expected[64],output[64];long_input[0]=0xf40;expected[0]=1;
    for(u32 i=1;i<64;++i){long_input[i]=0xf90;expected[i]=i>=31?1:0;}
    CHECK(boundaries({long_input,64},{expected,64}));
    const u32 symbol[]{0x1f600,0xf39,0xf71,0xf72};const u8 ye[]{1,0,0,1};CHECK(boundaries({symbol,4},{ye,4}));
    u32 selected[66];u8 selected_expected[66];selected[0]=0xf40;selected_expected[0]=1;
    for(u32 i=1;i<65;++i){selected[i]=i&1?0xf90:0xe0100;selected_expected[i]=i>=61&&(i&1)?1:0;}
    selected[65]=0xf40;selected_expected[65]=1;CHECK(boundaries({selected,66},{selected_expected,66}));
    for(auto& x:output)x=255;
    CHECK(sfnt_tibetan_cluster_boundaries({a,7},{output,6})==SfntStatus::output_too_small);
    for(u8 x:output)CHECK(x==255);
    const u32 invalid[]{0xf40,0xd800};
    CHECK(sfnt_tibetan_cluster_boundaries({invalid,2},{output,64})==SfntStatus::invalid_input);
    for(u8 x:output)CHECK(x==255);
    CHECK(sfnt_tibetan_cluster_boundaries({nullptr,1},{output,64})==SfntStatus::invalid_input);
    CHECK(sfnt_tibetan_cluster_boundaries({a,7},{nullptr,7})==SfntStatus::invalid_input);
    CHECK(sfnt_tibetan_cluster_boundaries({long_input,4},{reinterpret_cast<u8*>(long_input),4})==SfntStatus::invalid_input);
    CHECK(sfnt_tibetan_cluster_boundaries({}, {},0)==SfntStatus::ok);
    const u32 original[]{0x1f600,0xf40,0xf73,0xf76,0xf78,0xf81,0xf75,0xf77,0xf79};
    const SfntTibetanInputScalar expanded[]{{0x1f600,0},{0xf40,2},{0xf71,3},{0xf72,3},{0xfb2,4},{0xf80,4},
        {0xfb3,5},{0xf80,5},{0xf71,6},{0xf80,6},{0xf75,7},{0xf77,8},{0xf79,9}};
    SfntTibetanInputScalar actual[16];SfntTibetanInputResult result{};
    for(u64 budget=0;budget<=27;++budget){for(auto& v:actual)v={0xffffffffu,0xffffffffu};
        const auto status=sfnt_tibetan_expand_cluster({original,9},{actual,16},&result,budget);
        CHECK(status==(budget<27?SfntStatus::resource_limit:SfntStatus::ok));
        if(status==SfntStatus::ok)CHECK(result.scalar_count==13&&result.source_units==10&&bytes_equal(actual,expanded,sizeof(expanded)));
        else {CHECK(!result.scalar_count&&!result.source_units);for(const auto& v:actual)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);}
    }
    for(auto& v:actual)v={0xffffffffu,0xffffffffu};
    CHECK(sfnt_tibetan_expand_cluster({original,9},{actual,12},&result)==SfntStatus::output_too_small&&result.scalar_count==13&&result.source_units==10);
    for(const auto& v:actual)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);
    CHECK(sfnt_tibetan_expand_cluster({invalid,2},{actual,16},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    for(const auto& v:actual)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);
    CHECK(sfnt_tibetan_expand_cluster({reinterpret_cast<u32*>(actual),2},{actual,16},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_tibetan_expand_cluster({}, {},&result,0)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    CHECK(sfnt_tibetan_expand_cluster({}, {},nullptr)==SfntStatus::invalid_input);
    SYSTEM_INFO system{};GetSystemInfo(&system);const u32 page=system.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE,PAGE_NOACCESS));CHECK(memory);
    const bool committed=VirtualAlloc(memory,page,MEM_COMMIT,PAGE_READWRITE)&&VirtualAlloc(memory+page*2,page,MEM_COMMIT,PAGE_READWRITE);
    bool good=false;
    if(committed){auto* input=reinterpret_cast<u32*>(memory+page-sizeof(original));copy_bytes(input,original,sizeof(original));
        auto* target=reinterpret_cast<SfntTibetanInputScalar*>(memory+page*3-sizeof(expanded));
        good=sfnt_tibetan_expand_cluster({input,9},{target,13},&result)==SfntStatus::ok&&bytes_equal(target,expanded,sizeof(expanded));
        good=good&&sfnt_tibetan_cluster_boundaries({input,9},{memory+page*3-9,9})==SfntStatus::ok;
    }
    CHECK(VirtualFree(memory,0,MEM_RELEASE));CHECK(committed&&good);
    print("PASS Tibetan input; assertions=");number(checks);print("\n");return true;
}
bool read(void* destination,u32 count) noexcept {u8* out=static_cast<u8*>(destination);while(count){DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),out,count,&got,nullptr)||!got)return false;out+=got;count-=got;}return true;}
bool write(const void* source,u32 count) noexcept {const u8* in=static_cast<const u8*>(source);while(count){DWORD done=0;if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),in,count,&done,nullptr)||!done)return false;in+=done;count-=done;}return true;}
bool stream(bool expand) noexcept {
    u8 header[12];CHECK(read(header,12)&&bytes_equal(header,"MDVCL1\0\0",8));const u32 cases=byte_load_le32(header+8);CHECK(cases&&cases<=100000&&write(header,12));
    for(u32 row=0;row<cases;++row){u32 n=0,input[256];u8 boundaries[256];SfntTibetanInputScalar output[512];SfntTibetanInputResult result{};
        CHECK(read(&n,4)&&n&&n<=256&&read(input,n*4));
        if(expand){CHECK(sfnt_tibetan_expand_cluster({input,n},{output,512},&result)==SfntStatus::ok&&write(&result,sizeof(result))&&write(output,result.scalar_count*sizeof(output[0])));}
        else CHECK(sfnt_tibetan_cluster_boundaries({input,n},{boundaries,256})==SfntStatus::ok&&write(boundaries,n));
    }return true;
}
int run() noexcept {
    wchar_t mode[16];const DWORD n=GetEnvironmentVariableW(L"MAICORD_TIBETAN_INPUT_STREAM",mode,16);
    if(n)return n<16&&stream(mode[0]=='2')?0:2;
    return units()?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_tibetan_input_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
