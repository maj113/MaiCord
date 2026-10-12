#include "../src/render/glyph_effect_ordinals.hpp"
namespace maicord {namespace {
u32 checks=0;u64 color_calls=0;volatile u64 result_sink=0;
void print(const char* p) noexcept {u32 n=0;while(p[n])++n;DWORD wrote=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),p,n,&wrote,nullptr);}
void number(u64 n) noexcept {char text[32];u32 count=0;do{text[count++]=static_cast<char>('0'+n%10);n/=10;}while(n);while(count){const char c=text[--count];DWORD wrote=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),&c,1,&wrote,nullptr);}}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line ");number(__LINE__);print("\n");return false;}}while(0)
bool colored(void*,u32 glyph) noexcept {++color_calls;return glyph%11==7;}
// Independent transcription of the previous renderer, including its two loops.
u32 reference(const ShapedText& text,Utf8View source,u32 glyph) noexcept {
    u32 start=0,logical=0;for(u32 i=0;i<text.cluster_count;++i){const u32 mapped=text.clusters[i];if(mapped!=0xffff&&mapped<=glyph&&mapped>=start)start=mapped;}
    u16 previous=0xffff;u32 unit=0;
    for(u32 byte=0;byte<source.bytes&&unit<text.cluster_count;){const auto cp=utf8_decode_one(source.data+byte,source.bytes-byte);if(!cp.valid)break;
        const auto mapped=text.clusters[unit];if(mapped==start)break;
        const bool space=cp.codepoint<=0x20||cp.codepoint==0xa0||cp.codepoint==0x1680||(cp.codepoint>=0x2000&&cp.codepoint<=0x200a)||cp.codepoint==0x202f||cp.codepoint==0x205f||cp.codepoint==0x3000;
        const bool color=colored(nullptr,mapped);if(mapped!=previous&&mapped!=0xffff&&!space&&!color)++logical;
        previous=mapped;byte+=cp.width;unit+=cp.codepoint>0xffff?2u:1u;}
    return logical;
}
bool compare(ShapedText& text,Utf8View source) noexcept {
    CHECK(glyph_effect_prepare(text,source,colored,nullptr));
    for(u32 i=0;i<text.glyph_count;++i){CHECK(text.glyphs[i].reserved==reference(text,source,i));CHECK(text.glyphs[i].reserved==glyph_effect_ordinal(text,source,i,colored,nullptr));
        CHECK(text.glyphs[i].index==static_cast<u16>(i)&&text.glyphs[i].advance==3.25f&&text.glyphs[i].advance_offset==.25f&&text.glyphs[i].ascender_offset==-.5f);}
    return true;
}
bool run() noexcept {
    TextGlyph glyphs[96]{};u16 clusters[96]{};ShapedText text{};text.glyphs=glyphs;text.glyph_capacity=96;text.clusters=clusters;text.cluster_capacity=96;text.glyph_count=96;
    for(u32 i=0;i<96;++i)glyphs[i]={3.25f,.25f,-.5f,static_cast<u16>(i),0};
    const Utf8View samples[]{utf8_literal("AV office ffi  abc"),utf8_literal("A\xf0\x9f\x98\x80" "B\xcc\x81  Z"),utf8_literal("\xd7\x90\xd7\x91 AB \xd8\xa7\xd9\x84"),utf8_literal("A\xc2\xa0\xe1\x9a\x80\xe2\x80\x80\xe2\x80\xaf\xe2\x81\x9f\xe3\x80\x80" "B"),utf8_literal("abc\xff"),utf8_literal("")};
    u32 random=0x6143ab91;
    for(const auto sample:samples){for(u32 iteration=0;iteration<128;++iteration){text.cluster_count=iteration%48;
        for(u32 i=0;i<text.cluster_count;++i){random=random*1664525u+1013904223u;clusters[i]=(random&15)==0?0xffff:static_cast<u16>((random>>8)%100);}
        CHECK(compare(text,sample));}}
    // Maximum encodable ordinal and immediate out-of-range fallback, without
    // changing geometry or allocating any preparation storage.
    static TextGlyph large[65534];static u16 mapped[65534];static char source[65534];
    for(u32 i=0;i<65534;++i){source[i]='A';mapped[i]=static_cast<u16>(i);large[i]={3.25f,.25f,-.5f,static_cast<u16>(i),0};}
    ShapedText maximum{};maximum.glyphs=large;maximum.glyph_count=maximum.glyph_capacity=65534;maximum.clusters=mapped;maximum.cluster_count=65533;maximum.cluster_capacity=65534;
    const auto monochrome=+[](void*,u32) noexcept {return false;};CHECK(glyph_effect_prepare(maximum,{source,65533},monochrome,nullptr));CHECK(large[65532].reserved==65532&&large[65533].reserved==65532);
    maximum.cluster_count=65534;large[0].reserved=123;CHECK(!glyph_effect_prepare(maximum,{source,65534},monochrome,nullptr)&&large[0].reserved==123);
    CHECK(glyph_effect_ordinal(maximum,{source,65534},65533,monochrome,nullptr)==65533);
    // Paired operation cost includes preparation and every resulting ordinal.
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));print("{\"kind\":\"glyph_effect_cost\",\"frequency\":");number(frequency.QuadPart);print(",\"batches\":[");
    maximum.cluster_count=maximum.glyph_count=512;for(u32 b=0;b<6;++b){u64 ticks[2]{},cycles[2]{},calls[2]{},sums[2]{};
        for(u32 order=0;order<2;++order){const u32 side=order^(b&1);LARGE_INTEGER first{},last{};ULONG64 c0=0,c1=0;color_calls=0;CHECK(QueryThreadCycleTime(GetCurrentThread(),&c0)&&QueryPerformanceCounter(&first));
            u64 sum=0;for(u32 repeat=0;repeat<4;++repeat){if(side){CHECK(glyph_effect_prepare(maximum,{source,512},colored,nullptr));for(u32 g=0;g<512;++g)sum+=large[g].reserved;}
                else for(u32 g=0;g<512;++g)sum+=reference(maximum,{source,512},g);}
            CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&c1));result_sink=sum;sums[side]=sum;ticks[side]=last.QuadPart-first.QuadPart;cycles[side]=c1-c0;calls[side]=color_calls;}
        CHECK(sums[0]==sums[1]&&calls[1]==2048);
        if(b)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print(",");number(cycles[0]);print(",");number(cycles[1]);print(",");number(calls[0]);print(",");number(calls[1]);print("]");}
    print("]}\nchecks=");number(checks);print("\nPASS glyph effect ordinals\n");return true;
}
}}
extern "C" void maicord_glyph_effect_entry(){ExitProcess(maicord::run()?0:1);}
