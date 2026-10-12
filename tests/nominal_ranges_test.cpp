#include "maicord/text/nominal_ranges.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
#include "generated/nominal_range_fixture.inc"
u64 prefixes=0,ranges=0,budgets=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&written,nullptr);}
void number(u64 n) noexcept {char s[32];u32 count=0;do{s[count++]=static_cast<char>('0'+n%10);n/=10;}while(n);for(u32 i=0;i<count/2;++i){const char c=s[i];s[i]=s[count-i-1];s[count-i-1]=c;}DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,count,&written,nullptr);}
struct Reader {u32 at=8;bool word(u32* v) noexcept {if(sizeof(nominal_range_fixture)-at<4)return false;*v=byte_load_le32(nominal_range_fixture+at);at+=4;return true;}};
bool run() noexcept {
    Reader reader;
    for(u32 mode=0;mode<2;++mode){u32 count=0;if(!reader.word(&count))return false;
        for(u32 record=0;record<count;++record){u32 first=0,end=0,code=0;if(!reader.word(&first)||!reader.word(&end)||!reader.word(&code)||end>0x110000||first>=end)return false;
            for(u32 cp=first;code&&cp<end;++cp){u32 text[4]{97,98,98,98};const u32 index=mode?2:1,n=mode?4:3;text[index]=cp;
                SfntGsubGlyph glyphs[4]{{1},{1},{1},{1}};glyphs[index].glyph=cp<65536?1:0;SfntNominalRange result{};
                const auto status=sfnt_nominal_range({text,n},sfnt_glyph_view({glyphs,n}),{},2,&result);
                if(status!=SfntStatus::ok||result.utf16_units!=(code&127)||result.simple!=static_cast<bool>(code&128)){
                    print("FAIL profile scalar=");number(cp);print(" mode=");number(mode);print("\n");return false;}
                ++prefixes;
            }
        }
    }
    u32 cases=0;if(!reader.word(&cases))return false;
    for(u32 c=0;c<cases;++c){u32 count=0,expected_count=0,text[256],expected[256][3];SfntGsubGlyph glyphs[256];
        if(!reader.word(&count)||!reader.word(&expected_count)||!count||count>256||expected_count>256)return false;
        for(u32 i=0;i<count;++i){if(!reader.word(&text[i]))return false;glyphs[i]={static_cast<u16>(text[i]<65536?1:0),SfntGlyphClass::unassigned,0};}
        for(u32 i=0;i<expected_count;++i)for(u32 j=0;j<3;++j)if(!reader.word(&expected[i][j]))return false;
        u32 at=0,units=0;
        for(u32 i=0;i<expected_count;++i){SfntNominalRange result{};
            if(sfnt_nominal_range({text+at,count-at},sfnt_glyph_view({glyphs+at,count-at}),{},2,&result)!=SfntStatus::ok||!result.scalar_count||units!=expected[i][0]||result.utf16_units!=expected[i][1]||result.simple!=(expected[i][2]!=0))return false;
            const u32 work=result.examined;for(u32 budget=0;budget<=work;++budget){SfntNominalRange limited{};
                const auto status=sfnt_nominal_range({text+at,count-at},sfnt_glyph_view({glyphs+at,count-at}),{},2,&limited,budget);++budgets;
                if(limited.examined>budget||status!=(budget<work?SfntStatus::resource_limit:SfntStatus::ok)||(status!=SfntStatus::ok&&(limited.scalar_count||limited.utf16_units)))return false;}
            at+=result.scalar_count;units+=result.utf16_units;++ranges;
        }
        if(at!=count)return false;
    }
    u32 invalid[]{97,0xd800};SfntGsubGlyph glyphs[]{{1},{1}};SfntNominalRange output{};u64 bits=2;
    if(sfnt_nominal_range({invalid,2},sfnt_glyph_view({glyphs,2}),{},2,&output)!=SfntStatus::invalid_input||output.scalar_count)return false;
    invalid[1]=98;
    if(sfnt_nominal_range({invalid,2},sfnt_glyph_view({glyphs,2}),{&bits,1},2,&output)!=SfntStatus::ok||output.simple||output.scalar_count!=2)return false;
    if(sfnt_nominal_range({invalid,2},sfnt_glyph_view({glyphs,2}),{},1,&output)!=SfntStatus::glyph_out_of_range||output.scalar_count)return false;
    return reader.at==sizeof(nominal_range_fixture);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_nominal_ranges_entry() noexcept {
    using namespace maicord;const bool ok=run();print("prefixes=");number(prefixes);print("\nranges=");number(ranges);print("\nbudget_checks=");number(budgets);print(ok?"\nPASS native nominal ranges\n":"\nFAIL native nominal ranges\n");ExitProcess(ok?0:1);
}
