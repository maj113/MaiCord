// No argument runs the registered checks. Manual: MAICORD_MYANMAR_INPUT_STREAM=1|2 answers cluster
// queries from stdin for the Python oracles (QA only).
#include "maicord/text/myanmar_input.hpp"
#include "../src/text/myanmar_character.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
u32 checks=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char b[32];u32 n=0;do{b[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n){const char s[]{b[--n],0};print(s);}}
bool fail(u32 line) noexcept {print("FAIL Myanmar input line ");number(line);print("\n");return false;}
#define CHECK(x) do{++checks;if(!(x))return fail(__LINE__);}while(false)
bool boundaries(ConstSlice<u32> input,ConstSlice<u8> expected) noexcept {
    u8 output[256];CHECK(input.count==expected.count&&input.count<=256);
    for(u64 budget=0;budget<=input.count*16u;++budget){for(auto& c:output)c=255;
        const auto status=sfnt_myanmar_cluster_boundaries(input,{output,256},budget);
        CHECK(status==(budget<input.count*16u?SfntStatus::resource_limit:SfntStatus::ok));
        if(status==SfntStatus::ok)CHECK(bytes_equal(output,expected.data,expected.count));
        else for(u8 c:output)CHECK(c==255);
    }return true;
}
bool preparation() noexcept {
    const u32 input[]{0x1004,0x103a,0x1039,0x1000,0x1039,0x1000,0x103b,0x103c,0x103d,0x1031,0x1031,0x102d,0x102f,0x1036,0x102c,0x1036};
    const u8 order[]{9,10,7,3,0,1,2,4,5,6,8,11,13,12,14,15};
    SfntIndicInputScalar output[32];SfntIndicInputResult result{};
    for(u64 budget=0;budget<=16*64;++budget){for(auto& v:output)v={0xffffffffu,0xffffffffu};
        const auto status=sfnt_myanmar_prepare_input({input,16},{output,32},&result,budget);
        CHECK(status==(budget==16*64?SfntStatus::ok:SfntStatus::resource_limit));
        if(status==SfntStatus::ok){CHECK(result.scalar_count==16&&result.source_units==16);
            for(u32 i=0;i<16;++i)CHECK(output[i].scalar==input[order[i]]&&output[i].source==order[i]);}
        else{CHECK(!result.scalar_count&&!result.source_units);for(const auto& v:output)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);}
    }
    for(u32 capacity=0;capacity<16;++capacity){for(auto& v:output)v={0xffffffffu,0xffffffffu};
        CHECK(sfnt_myanmar_prepare_input({input,16},{output,capacity},&result)==SfntStatus::output_too_small&&result.scalar_count==16&&result.source_units==16);
        for(const auto& v:output)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);}
    const u32 broken[]{0x1031};CHECK(sfnt_myanmar_prepare_input({broken,1},{output,32},&result)==SfntStatus::ok&&result.scalar_count==2&&output[0].scalar==0x1031&&output[1].scalar==0x25cc&&!output[0].source&&!output[1].source);
    const u32 selectors[]{0x1000,0x1031,0xe0100,0xfe00};
    CHECK(sfnt_myanmar_prepare_input({selectors,4},{output,32},&result)==SfntStatus::ok&&result.scalar_count==4&&result.source_units==5);
    const SfntIndicInputScalar selected[]{{0x1031,1},{0xe0100,2},{0x1000,0},{0xfe00,4}};
    CHECK(bytes_equal(output,selected,sizeof(selected)));
    const u32 truncated[]{0x1004,0x103a,0x1039,0xe0100,0x104a};
    CHECK(sfnt_myanmar_prepare_input({truncated,5},{output,3},&result)==SfntStatus::output_too_small&&result.scalar_count==4&&result.source_units==6);
    CHECK(sfnt_myanmar_prepare_input({truncated,5},{output,4},&result)==SfntStatus::ok&&result.scalar_count==4);
    const SfntIndicInputScalar trunc_expected[]{{0,0},{0x1004,0},{0x103a,1},{0x104a,5}};
    CHECK(bytes_equal(output,trunc_expected,sizeof(trunc_expected)));
    for(auto& v:output)v={0xffffffffu,0xffffffffu};const u32 bad[]{0x1000,0xdfff};
    CHECK(sfnt_myanmar_prepare_input({bad,2},{output,32},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    CHECK(sfnt_myanmar_prepare_input({nullptr,2},{output,32},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_myanmar_prepare_input({input,16},{nullptr,16},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_myanmar_prepare_input({input,16},{output,32},nullptr)==SfntStatus::invalid_input);
    CHECK(sfnt_myanmar_prepare_input({reinterpret_cast<u32*>(output),16},{output,32},&result)==SfntStatus::invalid_input);
    for(const auto& v:output)CHECK(v.scalar==0xffffffffu&&v.source==0xffffffffu);
    CHECK(sfnt_myanmar_prepare_input({},{},&result,0)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE,PAGE_NOACCESS));CHECK(memory);
    const bool committed=VirtualAlloc(memory,page,MEM_COMMIT,PAGE_READWRITE)&&VirtualAlloc(memory+page*2,page,MEM_COMMIT,PAGE_READWRITE);bool exact=false;
    if(committed){auto* original=reinterpret_cast<u32*>(memory+page-sizeof(selectors));auto* target=reinterpret_cast<SfntIndicInputScalar*>(memory+page*3-sizeof(selected));
        copy_bytes(original,selectors,sizeof(selectors));DWORD old=0;
        exact=VirtualProtect(memory,page,PAGE_READONLY,&old)&&sfnt_myanmar_prepare_input({original,4},{target,4},&result)==SfntStatus::ok&&bytes_equal(target,selected,sizeof(selected));}
    CHECK(VirtualFree(memory,0,MEM_RELEASE)&&committed&&exact);return true;
}
bool units() noexcept {
    CHECK(preparation());
    const u32 a[]{0x1004,0x103a,0x1039,0x1000,0x1031};const u8 ae[]{1,0,0,0,0};
    const u32 b[]{0x1000,0x1036,0x102b,0x1000,0x102c,0x103c,0x103c};const u8 be[]{1,0,1,1,0,0,0};
    const u32 c[]{0x1039,0xfe00,0xfe00,0x1000,0x103a,0x103e};const u8 ce[]{1,0,1,1,0,1};
    CHECK(boundaries({a,5},{ae,5})&&boundaries({b,7},{be,7})&&boundaries({c,6},{ce,6}));
    u32 long_input[80];u8 expected[80],output[80];long_input[0]=0x1000;expected[0]=1;
    for(u32 i=1;i<80;++i){long_input[i]=0x1031;expected[i]=i>=31?1:0;}
    CHECK(boundaries({long_input,80},{expected,80}));
    for(auto& v:output)v=255;
    CHECK(sfnt_myanmar_cluster_boundaries({a,5},{output,4})==SfntStatus::output_too_small);
    const u32 bad[]{0x1000,0xd800},bad2[]{0x110000};
    CHECK(sfnt_myanmar_cluster_boundaries({bad,2},{output,80})==SfntStatus::invalid_input);
    CHECK(sfnt_myanmar_cluster_boundaries({bad2,1},{output,80})==SfntStatus::invalid_input);
    CHECK(sfnt_myanmar_cluster_boundaries({nullptr,1},{output,80})==SfntStatus::invalid_input);
    CHECK(sfnt_myanmar_cluster_boundaries({a,5},{nullptr,5})==SfntStatus::invalid_input);
    CHECK(sfnt_myanmar_cluster_boundaries({long_input,8},{reinterpret_cast<u8*>(long_input)+4,8})==SfntStatus::invalid_input);
    CHECK(sfnt_myanmar_cluster_boundaries({},{},0)==SfntStatus::ok);
    for(u8 v:output)CHECK(v==255);
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE,PAGE_NOACCESS));CHECK(memory);
    const bool committed=VirtualAlloc(memory,page,MEM_COMMIT,PAGE_READWRITE)&&VirtualAlloc(memory+page*2,page,MEM_COMMIT,PAGE_READWRITE);
    bool exact=false;
    if(committed){auto* input=reinterpret_cast<u32*>(memory+page-sizeof(a));auto* target=memory+page*3-5;
        copy_bytes(input,a,sizeof(a));DWORD old=0;
        exact=VirtualProtect(memory,page,PAGE_READONLY,&old)&&sfnt_myanmar_cluster_boundaries({input,5},{target,5})==SfntStatus::ok&&bytes_equal(target,ae,5);}
    CHECK(VirtualFree(memory,0,MEM_RELEASE)&&committed&&exact);
    print("PASS Myanmar input; assertions=");number(checks);print("\n");return true;
}
bool read(void* destination,u32 count) noexcept {u8* out=static_cast<u8*>(destination);while(count){DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),out,count,&got,nullptr)||!got)return false;out+=got;count-=got;}return true;}
bool write(const void* source,u32 count) noexcept {const u8* in=static_cast<const u8*>(source);while(count){DWORD done=0;if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),in,count,&done,nullptr)||!done)return false;in+=done;count-=done;}return true;}
bool stream(bool prepare) noexcept {
    u8 header[12];CHECK(read(header,12)&&bytes_equal(header,"MDVCL1\0\0",8));const u32 cases=byte_load_le32(header+8);
    if(prepare)copy_bytes(header,"MDVPR1\0\0",8);CHECK(cases&&cases<=100000&&write(header,12));
    for(u32 row=0;row<cases;++row){u32 n=0,input[256];u8 boundaries[256];SfntIndicInputScalar prepared[512];SfntIndicInputResult result{};
        CHECK(read(&n,4)&&n&&n<=256&&read(input,n*4));
        if(prepare){CHECK(sfnt_myanmar_prepare_input({input,n},{prepared,512},&result)==SfntStatus::ok&&write(&result,sizeof(result))&&write(prepared,result.scalar_count*sizeof(prepared[0])));}
        else CHECK(sfnt_myanmar_cluster_boundaries({input,n},{boundaries,256})==SfntStatus::ok&&write(boundaries,n));
    }return true;
}
int run() noexcept {
    wchar_t mode[16];const DWORD n=GetEnvironmentVariableW(L"MAICORD_MYANMAR_INPUT_STREAM",mode,16);
    if(n)return n<16&&stream(mode[0]=='2')?0:2;
    return units()?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_myanmar_input_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
