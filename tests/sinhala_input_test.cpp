// No argument runs the registered checks. Manual: MAICORD_SINHALA_INPUT_STREAM=1|2 answers cluster
// queries from stdin for the Python oracles (QA only).
#include "maicord/text/sinhala_input.hpp"
#include "../src/text/use_character.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
u32 checks=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char b[32];u32 n=0;do{b[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n){const char s[]{b[--n],0};print(s);}}
bool fail(u32 line) noexcept {print("FAIL Sinhala input line ");number(line);print("\n");return false;}
#define CHECK(x) do{++checks;if(!(x))return fail(__LINE__);}while(false)
bool boundaries(ConstSlice<u32> input,ConstSlice<u8> expected) noexcept {
    u8 output[256];CHECK(input.count==expected.count&&input.count<=256);
    for(u64 budget=0;budget<=input.count*4u;++budget){for(auto& c:output)c=255;
        const auto status=sfnt_sinhala_cluster_boundaries(input,{output,256},budget);
        CHECK(status==(budget<input.count*4u?SfntStatus::resource_limit:SfntStatus::ok));
        if(status==SfntStatus::ok)CHECK(bytes_equal(output,expected.data,expected.count));
        else for(u8 c:output)CHECK(c==255);
    }return true;
}
bool units() noexcept {
    const u32 a[]{0xd9a,0xdca,0xd9a,0xddd,0xdca,0x200d,0xfe00};const u8 ae[]{1,0,0,0,1,1,0};
    const u32 b[]{0xd85,0xdcf,0xd85,0xfe00,0xdcf,0xd85,0x200d,0xdcf};const u8 be[]{1,1,1,0,1,1,0,0};
    const u32 c[]{0x20,0x34f,0x200d,0x25cc,0xdca,0x25cc,0xdd9};const u8 ce[]{1,0,1,1,0,1,0};
    CHECK(boundaries({a,7},{ae,7}));CHECK(boundaries({b,8},{be,8}));CHECK(boundaries({c,7},{ce,7}));
    const u32 placeholders[]{0x2d,0xd7,0x2010,0x2011,0x2012,0x2013,0x2014,0x2015,0x2022,0x25fb,0x25fc,0x25fd,0x25fe};
    const u8 pe[]{1,0,0};for(u32 placeholder:placeholders){const u32 text[]{placeholder,0xdd9,0xdcf};CHECK(boundaries({text,3},{pe,3}));}
    const u32 symbol[]{0x24,0x34f,0x200d};CHECK(boundaries({symbol,3},{pe,3}));
    const u32 selectors[]{0x200c,0xfe00,0xfe01,0x200d};const u8 se[]{1,0,1,0};CHECK(boundaries({selectors,4},{se,4}));
    const u32 reserved[]{0xdf8,0xfe0f,0x200c};const u8 re[]{1,0,1};CHECK(boundaries({reserved,3},{re,3}));
    const u32 numeric[]{0xd9a,0xdca,0xdeb};CHECK(boundaries({numeric,3},{re,3}));
    const u32 number_base[]{0xdeb,0xdca,0xd9a};CHECK(boundaries({number_base,3},{pe,3}));
    u32 long_input[64];u8 expected[64],output[64];long_input[0]=0xd9a;expected[0]=1;
    for(u32 i=1;i<64;++i){long_input[i]=0xdd9;expected[i]=i>=31?1:0;}
    CHECK(boundaries({long_input,64},{expected,64}));
    for(auto& x:output)x=255;
    CHECK(sfnt_sinhala_cluster_boundaries({a,7},{output,6})==SfntStatus::output_too_small);
    for(u8 x:output)CHECK(x==255);
    const u32 invalid[]{0xd9a,0xd800};
    CHECK(sfnt_sinhala_cluster_boundaries({invalid,2},{output,64})==SfntStatus::invalid_input);
    for(u8 x:output)CHECK(x==255);
    CHECK(sfnt_sinhala_cluster_boundaries({nullptr,1},{output,64})==SfntStatus::invalid_input);
    CHECK(sfnt_sinhala_cluster_boundaries({a,7},{nullptr,7})==SfntStatus::invalid_input);
    CHECK(sfnt_sinhala_cluster_boundaries({long_input,4},{reinterpret_cast<u8*>(long_input),4})==SfntStatus::invalid_input);
    CHECK(sfnt_sinhala_cluster_boundaries({}, {},0)==SfntStatus::ok);
    const u32 original[]{0x1f600,0xd9a,0xdda,0xddc,0xddd,0xdde,0xd85};
    const SfntSinhalaInputScalar expanded[]{{0x1f600,0},{0xd9a,2},{0xdd9,3},{0xdca,3},{0xdd9,4},{0xdcf,4},
        {0xdd9,5},{0xddd,5},{0xdd9,6},{0xddf,6},{0xd85,7}};
    SfntSinhalaInputScalar actual[16];SfntSinhalaInputResult result{};
    for(u64 budget=0;budget<=21;++budget){for(auto& v:actual)v={0xffffffffu,0xffffffffu};
        const auto status=sfnt_sinhala_expand_cluster({original,7},{actual,16},&result,budget);
        CHECK(status==(budget<21?SfntStatus::resource_limit:SfntStatus::ok));
        if(status==SfntStatus::ok)CHECK(result.scalar_count==11&&result.source_units==8&&bytes_equal(actual,expanded,sizeof(expanded)));
        else {CHECK(!result.scalar_count&&!result.source_units);for(const auto& v:actual)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);}
    }
    for(auto& v:actual)v={0xffffffffu,0xffffffffu};
    CHECK(sfnt_sinhala_expand_cluster({original,7},{actual,10},&result)==SfntStatus::output_too_small&&result.scalar_count==11&&result.source_units==8);
    for(const auto& v:actual)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);
    CHECK(sfnt_sinhala_expand_cluster({invalid,2},{actual,16},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    for(const auto& v:actual)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);
    CHECK(sfnt_sinhala_expand_cluster({reinterpret_cast<u32*>(actual),2},{actual,16},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_sinhala_expand_cluster({}, {},&result,0)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    CHECK(sfnt_sinhala_expand_cluster({}, {},nullptr)==SfntStatus::invalid_input);
    SYSTEM_INFO system{};GetSystemInfo(&system);const u32 page=system.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE,PAGE_NOACCESS));CHECK(memory);
    const bool committed=VirtualAlloc(memory,page,MEM_COMMIT,PAGE_READWRITE)&&VirtualAlloc(memory+page*2,page,MEM_COMMIT,PAGE_READWRITE);
    bool good=false;
    if(committed){auto* input=reinterpret_cast<u32*>(memory+page-sizeof(original));copy_bytes(input,original,sizeof(original));
        auto* target=reinterpret_cast<SfntSinhalaInputScalar*>(memory+page*3-sizeof(expanded));
        good=sfnt_sinhala_expand_cluster({input,7},{target,11},&result)==SfntStatus::ok&&bytes_equal(target,expanded,sizeof(expanded));
        good=good&&sfnt_sinhala_cluster_boundaries({input,7},{memory+page*3-7,7})==SfntStatus::ok;
    }
    CHECK(VirtualFree(memory,0,MEM_RELEASE));CHECK(committed&&good);
    print("PASS Sinhala input; assertions=");number(checks);print("\n");return true;
}
bool read(void* destination,u32 count) noexcept {u8* out=static_cast<u8*>(destination);while(count){DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),out,count,&got,nullptr)||!got)return false;out+=got;count-=got;}return true;}
bool write(const void* source,u32 count) noexcept {const u8* in=static_cast<const u8*>(source);while(count){DWORD done=0;if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),in,count,&done,nullptr)||!done)return false;in+=done;count-=done;}return true;}
bool stream(bool expand) noexcept {
    u8 header[12];CHECK(read(header,12)&&bytes_equal(header,"MDVCL1\0\0",8));const u32 cases=byte_load_le32(header+8);CHECK(cases&&cases<=100000&&write(header,12));
    for(u32 row=0;row<cases;++row){u32 n=0,input[256];u8 boundaries[256];SfntSinhalaInputScalar output[512];SfntSinhalaInputResult result{};
        CHECK(read(&n,4)&&n&&n<=256&&read(input,n*4));
        if(expand){CHECK(sfnt_sinhala_expand_cluster({input,n},{output,512},&result)==SfntStatus::ok&&write(&result,sizeof(result))&&write(output,result.scalar_count*sizeof(output[0])));}
        else CHECK(sfnt_sinhala_cluster_boundaries({input,n},{boundaries,256})==SfntStatus::ok&&write(boundaries,n));
    }return true;
}
int run() noexcept {
    wchar_t mode[16];const DWORD n=GetEnvironmentVariableW(L"MAICORD_SINHALA_INPUT_STREAM",mode,16);
    if(n)return n<16&&stream(mode[0]=='2')?0:2;
    return units()?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_sinhala_input_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
