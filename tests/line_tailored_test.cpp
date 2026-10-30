#include "maicord/text/detail/line_break_tailored.hpp"
#include "maicord/core/byte_order.hpp"
#include "text_test_support.hpp"
namespace maicord {namespace {
u32 cases=0,checks=0;
bool compare(ConstSlice<u32> text,ConstSlice<u8> expected) noexcept {
    u8 out[258];if(text.count>256||expected.count!=text.count+1)return false;out[text.count+1]=0x5a;
    if(!line_breaks_tailored(text,{out,text.count+1})||out[text.count+1]!=0x5a)return false;
    ++cases;for(u32 i=0;i<expected.count;++i){++checks;if(out[i]!=expected[i]){print(utf8_literal("FAIL case="));number(cases);print(utf8_literal(" boundary="));number(i);print(utf8_literal("\n"));return false;}}return true;
}
template<usize N,usize M> bool sample(const u32 (&text)[N],const u8 (&expected)[M]) noexcept {return compare({text,static_cast<u32>(N)},{expected,static_cast<u32>(M)});}
bool units() noexcept {
    const u32 space[]{'a',' ',0xfe0f,0x301,' ','b'};const u8 space_breaks[]{0,0,0,1,0,1,1};
    const u32 newline[]{'a',10,0xfe0f,0x301,10,'b'};const u8 newline_breaks[]{0,0,0,2,0,2,1};
    const u32 final[]{'a',10,0xfe0f};const u8 final_breaks[]{0,0,0,2};
    const u32 joiner[]{0x2010,0x34f,0x308,'L'};const u8 joiner_breaks[]{0,1,0,0,1};
    const u32 regional[]{0x1f1e6,0x1f1e7,0x1f1e8};const u8 regional_breaks[]{0,0,0,1};
    const u32 modifier[]{0x4e2d,0x1f3fb,'a'};const u8 modifier_breaks[]{0,0,1,1};
    const u32 quotes[]{0xab,' ','a',' ',0xbb};const u8 quote_breaks[]{0,0,1,0,1,1};
    const u32 cjk[]{0x4e2d,0x1acf,'a'};const u8 cjk_breaks[]{0,0,1,1};
    const u32 selectors[]{'A',10,0xfe0f,0xfe0f,':','Q'};const u8 selector_breaks[]{0,0,0,2,0,0,1};
    if(!sample(space,space_breaks)||!sample(newline,newline_breaks)||!sample(final,final_breaks)||!sample(joiner,joiner_breaks)||
       !sample(regional,regional_breaks)||!sample(modifier,modifier_breaks)||!sample(quotes,quote_breaks)||!sample(cjk,cjk_breaks)||!sample(selectors,selector_breaks))return false;
    u8 out[8];for(auto& b:out)b=0x5a;
    if(line_breaks_tailored({nullptr,1},{out,8})||line_breaks_tailored({space,6},{nullptr,8})||
       line_breaks_tailored({space,6},{out,6})||line_breaks_tailored({space,~0u},{out,~0u}))return false;
    for(u8 b:out){++checks;if(b!=0x5a)return false;}
    if(!line_breaks_tailored({},{out,8})||out[0])return false;for(u32 i=1;i<8;++i){++checks;if(out[i]!=0x5a)return false;}
    return true;
}
bool corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    struct Close {HANDLE file;u8* bytes=nullptr;~Close() noexcept {if(bytes)VirtualFree(bytes,0,MEM_RELEASE);CloseHandle(file);}} close{file};Win32LargeInteger length{};
    if(!GetFileSizeEx(file,length.native())||length.QuadPart<12||length.QuadPart>32*1024*1024)return false;const u32 size=static_cast<u32>(length.QuadPart);
    close.bytes=static_cast<u8*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!close.bytes)return false;DWORD done=0;
    if(!ReadFile(file,close.bytes,size,&done,nullptr)||done!=size||!bytes_equal(close.bytes,"MCWINLB1",8))return false;
    const u32 count=byte_load_le32(close.bytes+8);if(!count)return false;u32 at=12,text[256];
    for(u32 c=0;c<count;++c){if(size-at<4)return false;const u32 n=byte_load_le32(close.bytes+at);at+=4;if(!n||n>256||n*5+1>size-at)return false;
        for(u32 i=0;i<n;++i)text[i]=byte_load_le32(close.bytes+at+i*4);at+=n*4;
        if(!compare({text,n},{close.bytes+at,n+1}))return false;at+=n+1;
    }return at==size;
}
bool long_contexts() noexcept {
    constexpr u32 n=300003;
    auto* text=static_cast<u32*>(VirtualAlloc(nullptr,n*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* out=static_cast<u8*>(VirtualAlloc(nullptr,n+1,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    struct Release {u32* text;u8* out;~Release() noexcept {if(out)VirtualFree(out,0,MEM_RELEASE);if(text)VirtualFree(text,0,MEM_RELEASE);}} release{text,out};
    if(!text||!out)return false;text[0]='a';text[1]=10;for(u32 i=2;i<n-2;++i)text[i]=0xfe0f;text[n-2]=0x301;text[n-1]='b';
    if(!line_breaks_tailored({text,n},{out,n+1}))return false;
    for(u32 i=0;i<=n;++i){++checks;if(out[i]!=(i==n?1:i==3?2:0))return false;}
    for(u32 i=0;i<n-1;++i)text[i]=i&1?0x301:0x1f1e6;text[n-1]='a';
    if(!line_breaks_tailored({text,n},{out,n+1}))return false;
    for(u32 i=0;i<=n;++i){++checks;if(out[i]!=(i>=n-1?1:0))return false;}
    return true;
}
bool property_contract() noexcept {
    u32 count=0;
    for(u32 cp=0;cp<=0x10ffff;++cp){const auto& p=*unicode_properties(cp);
        if(p.general_category()==UnicodeGeneralCategory::cn&&p.has(UnicodeFlag::extended_pictographic)){
            if(p.line_break()!=UnicodeLineBreak::id&&p.line_break()!=UnicodeLineBreak::xx)return false;++count;}}
    return count==1453;
}
bool run() noexcept {
    if(!units()||!long_contexts()||!property_contract())return false;
    const wchar_t* c=test_arguments();
    if(!*c)return true;
    wchar_t path[32768];return test_path_argument(c,path)&&corpus(path);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_line_tailored_entry() noexcept {
    const bool ok=maicord::run();maicord::print(maicord::utf8_literal("cases="));maicord::number(maicord::cases);maicord::print(maicord::utf8_literal(" checks="));maicord::number(maicord::checks);
    maicord::print(ok?maicord::utf8_literal(" PASS native line compatibility\n"):maicord::utf8_literal(" FAIL native line compatibility\n"));ExitProcess(ok?0:1);
}
