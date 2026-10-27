#include "maicord/text/tibetan_input.hpp"
#include "maicord/core/byte_order.hpp"
#include "test_output.hpp"
namespace maicord {namespace {
u32 checks=0;
bool fail(u32 line) noexcept {test_write_text("FAIL Tibetan input line ");test_write_u64(line);test_write_text("\n");return false;}
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
    Win32SystemInfo system{};GetSystemInfo(system.native());const u32 page=system.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE,PAGE_NOACCESS));CHECK(memory);
    const bool committed=VirtualAlloc(memory,page,MEM_COMMIT,PAGE_READWRITE)&&VirtualAlloc(memory+page*2,page,MEM_COMMIT,PAGE_READWRITE);
    bool good=false;
    if(committed){auto* input=reinterpret_cast<u32*>(memory+page-sizeof(a));copy_bytes(input,a,sizeof(a));
        good=sfnt_tibetan_cluster_boundaries({input,7},{memory+page*3-7,7})==SfntStatus::ok;
    }
    CHECK(VirtualFree(memory,0,MEM_RELEASE));CHECK(committed&&good);
    test_write_text("PASS Tibetan input; assertions=");test_write_u64(checks);test_write_text("\n");return true;
}
int run() noexcept {return units()?0:1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_tibetan_input_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
