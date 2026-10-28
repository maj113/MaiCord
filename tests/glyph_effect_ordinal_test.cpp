#include "../src/render/glyph_effect_ordinals.hpp"
#include "maicord/render/text_effect.hpp"
#include "test_output.hpp"
namespace maicord {namespace {
u32 checks=0;u64 color_calls=0;
#define CHECK(x) do{++checks;if(!(x)){test_write_text("FAIL line ");test_write_u64(__LINE__);test_write_text("\n");return false;}}while(0)
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
    TextEffectConstants uploaded{};u32 uploaded_count=0,uploads=0;
    const auto frame=[&](u32 count,bool animated,float seconds,float scale) noexcept {
        if(text_effect_upload_required(uploaded,uploaded_count,count,animated,seconds,scale)){
            ++uploads;uploaded_count=count;uploaded.seconds=seconds;uploaded.scale=scale;}
    };
    for(u32 i=0;i<100;++i)frame(128,false,static_cast<float>(i),1);
    frame(64,false,100,1);frame(128,false,101,1);
    CHECK(uploads==1); // Static redraws and retained prefixes share one upload.
    uploaded_count=0;frame(1,false,102,1);frame(128,false,103,1);
    CHECK(uploads==3); // Material edits and growth after discard refresh the buffer.
    frame(128,true,104,1);frame(128,true,104,1);frame(128,true,0,1);
    frame(128,true,0,2);frame(128,false,105,2);frame(128,false,106,1);
    CHECK(uploads==7); // Animated ticks, clock wrap and DPI changes remain visible.
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
    // One deterministic pass checks the preparation work and every resulting ordinal.
    maximum.cluster_count=maximum.glyph_count=512;color_calls=0;
    CHECK(glyph_effect_prepare(maximum,{source,512},colored,nullptr));
    CHECK(color_calls==512);
    for(u32 glyph=0;glyph<512;++glyph)
        CHECK(large[glyph].reserved==reference(maximum,{source,512},glyph));
    test_write_text("checks=");test_write_u64(checks);test_write_text("\nPASS glyph effect ordinals\n");return true;
}
}}
extern "C" void maicord_glyph_effect_entry(){ExitProcess(maicord::run()?0:1);}
