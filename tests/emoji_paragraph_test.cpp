#include "maicord/text/emoji_paragraph.hpp"
#include "maicord/text/visibility.hpp"
#include "maicord/text/detail/paragraph_layout.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
#include "emoji_test_support.inc"
#include "generated/emoji_paragraph_fixture.inc"
NativeLineCluster line_clusters[256];NativeParagraphCluster paragraph_clusters[256];
u16 paragraph_glyphs[1024];SfntPlacement paragraph_placements[1024];u32 paragraph_map[256],scalars[256];u8 breaks[257];
NativeParagraphRun layout_runs[256];NativeParagraphHitCluster hit_clusters[256];NativeParagraphHitLine hit_lines[257];float hit_advances[1024];
u32 bit_pattern(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);return bits;}
float real(u32 value) noexcept {float result=0;copy_bytes(&result,&value,4);return result;}
bool word_equal(Reader& r,u32 actual) noexcept {u32 expected=0;CHECK(r.word(&expected));EQUAL(actual,expected);return true;}
NativeAsciiParagraphStorage buffers(u32 mode) noexcept {return {{mode?ascii_buffers.shared():ascii_buffers.storage(),{line_clusters,256}},
    {paragraph_glyphs,1024},{paragraph_placements,1024},{paragraph_map,256},{paragraph_clusters,256}};}
bool visibility() noexcept {
    // Frozen exhaustive host observations, independently expressed as intervals.
    constexpr u32 ranges[][2]{{0,31},{127,159},{173,173},{8203,8203},{8206,8207},{8232,8238},{8288,8292},{8294,8303},{65279,65279},
        {65529,65531},{65534,65535},{113824,113827},{119155,119162},{917505,917505},{917536,917631}};
    u32 at=0,hidden=0;for(u32 cp=0;cp<0x110000;++cp){while(at<15&&cp>ranges[at][1])++at;
        const bool expected=at<15&&cp>=ranges[at][0];EQUAL(native_text_nonvisual(cp)?1u:0u,expected?1u:0u);hidden+=expected?1u:0u;}
    CHECK(hidden==206&&!native_text_nonvisual(0x110000)&&!native_text_nonvisual(0xffffffff));return true;
}
bool guards(const NativeEmojiPlan& plan) noexcept {
    {
        NativeParagraphShape shape{3,2,3,{10,8}};
        NativeParagraphCluster clusters[]{{0,1,0,1,5,0},{1,2,1,0,0,native_paragraph_newline},{2,3,1,1,6,0}};
        SfntPlacement placements[]{{5,0,0},{6,0,0}};SfntFontLineParts style{};style.baseline=8;style.descent=2;
        NativeParagraphLine lines[2]{};NativeParagraphLayout layout{};
        CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,3},{placements,2},{&style,1},100,40,true,{layout_runs,256},&layout,{lines,2},nullptr,{},10)==SfntStatus::ok);
        CHECK(layout.line_count==2&&layout.run_count==2&&layout.height==10&&layout.width==5&&layout.flags==native_paragraph_height_trimmed);
        CHECK(lines[0].height==10&&lines[1].height==0&&!lines[1].run_count&&lines[1].top==10);
        CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,3},{placements,2},{&style,1},100,40,true,{layout_runs,256},&layout,{lines,1},nullptr,{},10)==SfntStatus::output_too_small&&!layout.line_count);
        NativeParagraphHits hits{};hits.source_units=99;NativeParagraphHitStorage hs{};hs.output=&hits;
        CHECK(native_prepared_paragraph_layout(shape,{clusters,3},{placements,2},100,40,true,{layout_runs,256},&layout,&hs,10)==SfntStatus::invalid_input&&!layout.line_count&&!hits.source_units);
        const u32 invalid_heights[]{0xbf800000u,0x7f800000u,0x7fc00000u};
        for(u32 bits:invalid_heights)CHECK(native_prepared_paragraph_layout(shape,{clusters,3},{placements,2},100,40,true,{layout_runs,256},&layout,nullptr,real(bits))==SfntStatus::invalid_input&&!layout.line_count);
        CHECK(native_prepared_paragraph_layout(shape,{clusters,3},{placements,2},100,40,true,{layout_runs,256},&layout,nullptr,0)==SfntStatus::ok&&layout.line_count==2&&layout.height==20&&layout.width==6&&!layout.flags);
    }
    auto s=buffers(0);NativeParagraphShape out{};const NativeLatinBoundaryStorage b{{scalars,256},{breaks,257}};
    CHECK(native_emoji_paragraph_shape(store,plan,{},17.5f,s,b,&out)==SfntStatus::ok&&!out.glyph_count&&!out.source_units&&out.vertical.height>0);
    CHECK(native_emoji_paragraph_shape(store,plan,{},0,s,b,&out)==SfntStatus::invalid_input&&!out.vertical.height);
    CHECK(native_emoji_paragraph_shape(store,plan,{},65537,s,b,&out)==SfntStatus::invalid_input);
    auto invalid=plan;invalid.cmap=nullptr;CHECK(native_emoji_paragraph_shape(store,invalid,{},17.5f,s,b,&out)==SfntStatus::invalid_input);
    CHECK(native_emoji_paragraph_shape(store,plan,{nullptr,1},17.5f,s,b,&out)==SfntStatus::invalid_input);
    CHECK(native_emoji_paragraph_shape(store,plan,utf8_literal("\xff"),17.5f,s,b,&out)==SfntStatus::invalid_input);
    CHECK(native_emoji_paragraph_shape(store,plan,utf8_literal("\xc2\xad"),17.5f,s,b,&out)==SfntStatus::unsupported_sequence);
    CHECK(native_emoji_paragraph_shape(store,plan,utf8_literal("\xe2\x80\x8f"),17.5f,s,b,&out)==SfntStatus::unsupported_sequence);
    CHECK(native_emoji_paragraph_shape(store,plan,{},17.5f,s,b,nullptr)==SfntStatus::invalid_input);
    const auto tag=utf8_literal("\xf3\xa0\x81\xa7");s.work={};s.glyphs={};s.placements={};
    CHECK(native_emoji_paragraph_shape(store,plan,tag,17.5f,s,b,&out,true)==SfntStatus::ok&&out.source_units==2&&out.cluster_count==1&&!out.glyph_count);
    CHECK(paragraph_map[0]==0xffffffffu&&paragraph_map[1]==0xffffffffu&&paragraph_clusters[0].flags==native_paragraph_nonvisual);
    s.clusters.count=0;CHECK(native_emoji_paragraph_shape(store,plan,tag,17.5f,s,b,&out)==SfntStatus::output_too_small&&!out.source_units);
    s=buffers(0);s.source_glyphs.count=1;CHECK(native_emoji_paragraph_shape(store,plan,tag,17.5f,s,b,&out)==SfntStatus::output_too_small);
    s=buffers(0);s.clusters.data=nullptr;CHECK(native_emoji_paragraph_shape(store,plan,tag,17.5f,s,b,&out)==SfntStatus::invalid_input);
    s=buffers(0);CHECK(native_emoji_paragraph_shape(store,plan,tag,17.5f,s,{},&out)==SfntStatus::output_too_small&&!out.cluster_count);
    return true;
}
bool corpus(ConstSlice<u8> data) noexcept {
    CHECK(data.count>=12&&bytes_equal(data.data,"MCEMPAR1",8));Reader reader{data,8};u32 font_count=0;CHECK(reader.word(&font_count));
    for(font_index=0;font_index<font_count;++font_index){u32 embedded=0,size=0,count=0,weight=0;ConstSlice<u8> font{};
        CHECK(reader.word(&embedded)&&reader.word(&size)&&reader.word(&count)&&reader.word(&weight)&&reader.take(size,&font));const wchar_t* path=temporary;
        if(embedded){CHECK(embedded==1&&write_font(font));}
        else{CHECK(size<32768);const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(font.data),static_cast<int>(size),font_path,32767);CHECK(n>0);font_path[n]=0;path=font_path;}
        NativeRunPlanHandle handle{};CHECK(acquire(path,&handle,false,weight));arena_reset(&state);NativeEmojiPlan plan{};
        CHECK(native_emoji_plan_prepare(store,handle,&state,&scratch,&plan)==SfntStatus::ok);root_bytes+=state.offset;if(!font_index)CHECK(guards(plan));
        for(case_index=0;case_index<count;++case_index){u32 em=0,bytes=0,glyph_count=0,units=0,points=0,ranges=0,run_count=0,line_count=0;ConstSlice<u8> text{},glyphs{},placements{},map{};
            CHECK(reader.word(&em)&&reader.word(&bytes)&&reader.word(&glyph_count)&&reader.word(&units)&&reader.word(&points)&&reader.word(&ranges)&&reader.word(&run_count)&&reader.word(&line_count));
            CHECK(glyph_count<=1024&&units<=256&&reader.take(bytes,&text)&&reader.take(glyph_count*2,&glyphs)&&reader.take(glyph_count*12,&placements)&&reader.take(units*4,&map));
            const auto geometry=reader.at;u32 next=0;
            for(u32 mode=0;mode<2;++mode){reader.at=geometry;auto s=buffers(mode);NativeParagraphShape shape{};NativeParagraphLayout layout{};NativeParagraphHits hits{};
                NativeParagraphHitStorage hs{{hit_clusters,256},{hit_lines,257},{hit_advances,1024},&hits,{}};
                EQUAL(static_cast<u32>(native_emoji_paragraph_shape(store,plan,{reinterpret_cast<const char*>(text.data),bytes},real(em),s,{{scalars,256},{breaks,257}},&shape)),static_cast<u32>(SfntStatus::ok));
                EQUAL(shape.glyph_count,glyph_count);EQUAL(shape.source_units,units);
                const auto fit=native_prepared_paragraph_layout(shape,{paragraph_clusters,shape.cluster_count},{paragraph_placements,shape.glyph_count},1000000,real(em)*4,true,{layout_runs,256},&layout,&hs);
                EQUAL(static_cast<u32>(fit),static_cast<u32>(SfntStatus::ok));
                EQUAL(layout.line_count,line_count);
                for(u32 i=0;i<glyph_count;++i){EQUAL(paragraph_glyphs[i],byte_load_le16(glyphs.data+i*2));u32 bits[3];copy_bytes(bits,paragraph_placements+i,12);for(u32 k=0;k<3;++k)EQUAL(bits[k],byte_load_le32(placements.data+(i*3+k)*4));}
                for(u32 i=0;i<units;++i)EQUAL(paragraph_map[i],byte_load_le32(map.data+i*4));
                CHECK(word_equal(reader,bit_pattern(layout.width))&&word_equal(reader,bit_pattern(layout.extent))&&word_equal(reader,bit_pattern(layout.height))&&word_equal(reader,bit_pattern(shape.vertical.height))&&word_equal(reader,bit_pattern(shape.vertical.baseline)));
                CHECK(word_equal(reader,hits.clusters.count));for(u32 i=0;i<hits.clusters.count;++i){const auto& c=hit_clusters[i];CHECK(word_equal(reader,c.source_end-c.source_first)&&word_equal(reader,bit_pattern(c.width)));}
                for(u32 at=0;at<=units;++at)for(u32 trailing=0;trailing<2;++trailing){NativeParagraphHitMetric hit{};float x=0;CHECK(native_paragraph_hit_position(hits,at,trailing!=0,&hit,&x)==SfntStatus::ok);
                    const u32 fields[]{at,trailing,hit.source_first,hit.source_count,bit_pattern(x),bit_pattern(hit.top),bit_pattern(hit.left),bit_pattern(hit.top),bit_pattern(hit.width),bit_pattern(hit.height),0,1,0};
                    for(u32 value:fields)CHECK(word_equal(reader,value));}
                for(u32 i=0;i<points;++i){u32 x=0,y=0;CHECK(reader.word(&x)&&reader.word(&y));NativeParagraphHitPoint hit{};CHECK(native_paragraph_hit_point(hits,real(x),real(y),&hit)==SfntStatus::ok);CHECK(word_equal(reader,hit.source_position)&&word_equal(reader,hit.inside?1u:0u));}
                for(u32 i=0;i<ranges;++i){u32 at=0,length=0,actual=0;CHECK(reader.word(&at)&&reader.word(&length));NativeParagraphHitMetric rectangles[16]{};
                    if(length)CHECK(native_paragraph_hit_range(hits,at,length,0,0,{rectangles,16},&actual)==SfntStatus::ok);CHECK(word_equal(reader,actual));
                    for(u32 j=0;j<actual;++j){const auto& m=rectangles[j];const u32 fields[]{m.source_first,m.source_count,bit_pattern(m.left),bit_pattern(m.top),bit_pattern(m.width),bit_pattern(m.height),0,1,0};for(u32 value:fields)CHECK(word_equal(reader,value));}}
                if(run_count!=0xffffffffu){EQUAL(layout.run_count,run_count);for(u32 i=0;i<run_count;++i){const auto& r=layout_runs[i];const u32 fields[]{r.source_first,r.source_count,r.glyph_first,r.glyph_count,0,bit_pattern(r.x),bit_pattern(r.baseline)};for(u32 value:fields)CHECK(word_equal(reader,value));}}
                if(!mode)next=reader.at;else EQUAL(reader.at,next);++runs;
            }++cases;
        }
        CHECK(native_run_plan_release(&store,handle)&&!native_font_stats(fonts).files);
    }
    CHECK(reader.at==data.count&&runs==cases*2);return true;
}
bool run() noexcept {
    CHECK(visibility());CHECK(arena_initialize(&scratch,16*1024*1024,0,4096)&&arena_initialize(&state,1024*1024,0,4096));wchar_t folder[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH,folder)&&GetTempFileNameW(folder,L"mcp",0,temporary));const wchar_t* command=GetCommandLineW();CHECK(argument(&command,font_path,32768));
    if(!argument(&command,font_path,32768))return corpus({emoji_paragraph_fixture,sizeof(emoji_paragraph_fixture)});
    ConstSlice<u8> data;return load_input(&data)&&corpus(data);
}
}}
extern "C" void maicord_emoji_paragraph_entry() noexcept {
    using namespace maicord;bool ok=emoji_cleanup(run());
    print("cases=");number(cases);print("\nruns=");number(runs);print("\nchecks=");number(checks);print("\nvalues=");number(values);print("\ndigest=");number(digest);
    print(ok?"\nPASS native emoji itemized paragraphs\n":"\nFAIL native emoji itemized paragraphs\n");ExitProcess(ok?0u:1u);
}
