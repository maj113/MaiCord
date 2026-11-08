#include "maicord/text/unicode/detail/line_break.hpp"
#include "../../support/text_test_support.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "../../support/test_output.hpp"
namespace maicord {namespace {
#include "../../generated/line_break_test.inc"
u32 cases=0,checks=0;
bool compare(ConstSlice<u32> text,ConstSlice<u8> expected) noexcept {
    u8 out[258];if(text.count>256||expected.count!=text.count+1)return false;out[text.count+1]=0x5a;
    if(!unicode_line_breaks(text,{out,text.count+1})||out[text.count+1]!=0x5a)return false;
    ++cases;for(u32 i=0;i<expected.count;++i){++checks;if(out[i]!=expected[i]){print(utf8_literal("FAIL case="));number(cases);print(utf8_literal(" boundary="));number(i);print(utf8_literal("\n"));return false;}}return true;
}
template<usize N,usize M> bool sample(const u32 (&text)[N],const u8 (&expected)[M]) noexcept {return compare({text,static_cast<u32>(N)},{expected,static_cast<u32>(M)});}
// Every case of the Unicode 17.0.0 LineBreakTest.txt: ÷ is an allowed or
// mandatory break (1 or 2), × none (0). Prints the passing count and the
// first failures.
bool vectors() noexcept {
    u32 at=0,count=0,failed=0;u32 text[256];u8 out[257];bool expected[257];
    while(at<sizeof(line_break_vectors)){const u32 n=line_break_vectors[at++];if(!n||sizeof(line_break_vectors)-at<n*3)return false;
        for(u32 i=0;i<n;++i,at+=3){const u32 value=line_break_vectors[at]|static_cast<u32>(line_break_vectors[at+1])<<8|static_cast<u32>(line_break_vectors[at+2])<<16;
            text[i]=value&0x1fffff;expected[i]=(value&0x800000)!=0;}
        expected[n]=true;++count;
        if(!unicode_line_breaks({text,n},{out,n+1}))return false;
        for(u32 i=0;i<=n;++i){++checks;
            if((out[i]!=0)!=expected[i]){if(++failed<=10){print(utf8_literal("FAIL LineBreakTest case="));number(count);print(utf8_literal(" boundary="));number(i);print(utf8_literal("\n"));}break;}}
    }
    cases+=count;print(utf8_literal("LineBreakTest passed="));number(count-failed);print(utf8_literal(" of "));number(count);print(utf8_literal("\n"));
    return !failed&&count==line_break_vector_count;
}
bool units() noexcept {
    // LB9, LB10: a selector after a space or newline is a lone mark (AL); one after a base joins it.
    const u32 space[]{'a',' ',0xfe0f,0x301,' ','b'};const u8 space_breaks[]{0,0,1,0,0,1,1};
    const u32 newline[]{'a',10,0xfe0f,0x301,10,'b'};const u8 newline_breaks[]{0,0,2,0,0,2,1};
    const u32 final[]{'a',10,0xfe0f};const u8 final_breaks[]{0,0,2,1};
    const u32 selectors[]{'A',10,0xfe0f,0xfe0f,':','Q'};const u8 selector_breaks[]{0,0,2,0,0,0,1};
    // LB20a: no break after a word-initial hyphen (CGJ and U+0308 join it, LB9).
    const u32 hyphen[]{0x2010,0x34f,0x308,'L'};const u8 hyphen_breaks[]{0,0,0,0,1};
    // LB30a: regional indicators pair up.
    const u32 regional[]{0x1f1e6,0x1f1e7,0x1f1e8};const u8 regional_breaks[]{0,0,1,1};
    // LB30b: EB × EM; after another ideograph an emoji modifier may break.
    const u32 modifier[]{0x1f466,0x1f3fb,0x4e2d,0x1f3fb};const u8 modifier_breaks[]{0,0,1,1,1};
    // LB15a, LB15b: spaces inside « » do not break.
    const u32 quotes[]{0xab,' ','a',' ',0xbb};const u8 quote_breaks[]{0,0,0,0,0,1};
    const u32 cjk[]{0x4e2d,0x1acf,'a'};const u8 cjk_breaks[]{0,0,1,1};
    // LB30: a fullwidth parenthesis may break from a letter, an ASCII one not.
    const u32 parentheses[]{'a',0xff08,'a','('};const u8 parenthesis_breaks[]{0,1,0,0,1};
    // LB15c: break before a decimal mark after a space ("subtract .5").
    const u32 decimal[]{'x',' ','.','5'};const u8 decimal_breaks[]{0,0,1,0,1};
    // LB25: PR × OP NU; LB31 when no number follows.
    const u32 price[]{'$','(','5'};const u8 price_breaks[]{0,0,0,1};
    const u32 word[]{'$','(','a'};const u8 word_breaks[]{0,1,0,1};
    // LB24: (AL|HL) × (PR|PO).
    const u32 affix[]{'a','$'};const u8 affix_breaks[]{0,0,1};
    // LB28a: orthographic syllables (Balinese, Batak): AK ÷ AK, AK VI × AK, AS × AS VF.
    const u32 aksara[]{0x1b13,0x1b13};const u8 aksara_breaks[]{0,1,1};
    const u32 virama[]{0x1b13,0x1b44,0x1b13};const u8 virama_breaks[]{0,0,0,1};
    const u32 killer[]{0x1bc0,0x1bc0,0x1bf3};const u8 killer_breaks[]{0,0,0,1};
    const u32 dotted[]{0x1b13,0x1b44,0x25cc,0x1b44,'a'};const u8 dotted_breaks[]{0,0,0,0,1,1};
    // LB8a, LB10: a ZWJ after a space is A, and no break follows it.
    const u32 joiner[]{' ',0x200d,0x4e00};const u8 joiner_breaks[]{0,1,0,1};
    if(!sample(space,space_breaks)||!sample(newline,newline_breaks)||!sample(final,final_breaks)||!sample(selectors,selector_breaks)||
       !sample(hyphen,hyphen_breaks)||!sample(regional,regional_breaks)||!sample(modifier,modifier_breaks)||!sample(quotes,quote_breaks)||
       !sample(cjk,cjk_breaks)||!sample(parentheses,parenthesis_breaks)||!sample(decimal,decimal_breaks)||!sample(price,price_breaks)||
       !sample(word,word_breaks)||!sample(affix,affix_breaks)||!sample(aksara,aksara_breaks)||!sample(virama,virama_breaks)||
       !sample(killer,killer_breaks)||!sample(dotted,dotted_breaks)||!sample(joiner,joiner_breaks))return false;
    u8 out[8];for(auto& b:out)b=0x5a;
    if(unicode_line_breaks({nullptr,1},{out,8})||unicode_line_breaks({space,6},{nullptr,8})||
       unicode_line_breaks({space,6},{out,6})||unicode_line_breaks({space,~0u},{out,~0u}))return false;
    for(u8 b:out){++checks;if(b!=0x5a)return false;}
    if(!unicode_line_breaks({},{out,8})||out[0])return false;for(u32 i=1;i<8;++i){++checks;if(out[i]!=0x5a)return false;}
    return true;
}
bool long_contexts() noexcept {
    constexpr u32 n=300003;
    auto* text=static_cast<u32*>(pages_allocate(n*4));
    auto* out=static_cast<u8*>(pages_allocate(n+1));
    struct Release {u32* text;u8* out;~Release() noexcept {if(out)pages_release(out);if(text)pages_release(text);}} release{text,out};
    if(!text||!out)return false;text[0]='a';text[1]=10;for(u32 i=2;i<n-2;++i)text[i]=0xfe0f;text[n-2]=0x301;text[n-1]='b';
    if(!unicode_line_breaks({text,n},{out,n+1}))return false;
    for(u32 i=0;i<=n;++i){++checks;if(out[i]!=(i==n?1:i==2?2:0))return false;}
    // Regional indicators pair up across the marks they carry.
    for(u32 i=0;i<n-1;++i)text[i]=i&1?0x301:0x1f1e6;text[n-1]='a';
    if(!unicode_line_breaks({text,n},{out,n+1}))return false;
    for(u32 i=0;i<=n;++i){++checks;if(out[i]!=(i>=n-1||(i&&i%4==0)?1:0))return false;}
    // Every boundary between aksaras waits for a following VF.
    for(u32 i=0;i<n-1;++i)text[i]=0x1b13;text[n-1]=0x1bf3;
    if(!unicode_line_breaks({text,n},{out,n+1}))return false;
    for(u32 i=0;i<=n;++i){++checks;if(out[i]!=(i==n||(i&&i<n-2)?1:0))return false;}
    return true;
}
// LB30b reads [\p{Extended_Pictographic}&\p{Cn}]; LineBreak.txt gives those
// code points ID (or XX, resolved to AL by LB1).
bool property_contract() noexcept {
    u32 count=0;
    for(u32 cp=0;cp<=0x10ffff;++cp){const auto& p=*unicode_properties(cp);
        if(p.general_category()==UnicodeGeneralCategory::cn&&p.has(UnicodeFlag::extended_pictographic)){
            if(p.line_break()!=UnicodeLineBreak::id&&p.line_break()!=UnicodeLineBreak::xx)return false;++count;}}
    return count==1453;
}
bool run() noexcept {return vectors()&&units()&&long_contexts()&&property_contract();}
}}
MAICORD_TEST_ENTRY(maicord_line_break_entry) {
    const bool ok=maicord::run();maicord::print(maicord::utf8_literal("cases="));maicord::number(maicord::cases);maicord::print(maicord::utf8_literal(" checks="));maicord::number(maicord::checks);
    maicord::print(ok?maicord::utf8_literal(" PASS native line breaks\n"):maicord::utf8_literal(" FAIL native line breaks\n"));::maicord::test_process_exit(ok?0:1);
}
