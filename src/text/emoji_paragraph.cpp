#include "maicord/text/emoji_paragraph.hpp"
#include "maicord/text/visibility.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/core/utf.hpp"
namespace maicord {
SfntStatus native_emoji_paragraph_shape(const NativeRunPlanStore& store,const NativeEmojiPlan& plan,Utf8View input,float em,
    const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,bool coverage,bool rtl,bool arabic) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};if(input.bytes>65535)return SfntStatus::resource_limit;
    if(!plan.cmap||(plan.substitution_roots.count&&!plan.substitution_roots.data)||(plan.positioning_roots.count&&!plan.positioning_roots.data)||em>65536||
       (input.bytes&&!input.data)||(s.source_glyphs.count&&!s.source_glyphs.data)||(s.clusters.count&&!s.clusters.data)||
       (s.glyphs.count&&!s.glyphs.data)||(s.placements.count&&!s.placements.data))return SfntStatus::invalid_input;
    NativeParagraphShape shaped{};const auto* metrics=native_run_plan_metrics(store,plan.run);if(!metrics)return SfntStatus::invalid_input;
    auto status=sfnt_single_font_line(*metrics,em,&shaped.vertical);if(status!=SfntStatus::ok)return status;
    for(u32 at=0;at<input.bytes;){const auto cp=utf8_decode_one(input.data+at,input.bytes-at);if(!cp.valid)return SfntStatus::invalid_input;
        if(native_text_nonvisual(cp.codepoint)){
            if(cp.codepoint==0xad||cp.codepoint==0x200f||cp.codepoint-0x202au<5||cp.codepoint-0x2066u<4)return SfntStatus::unsupported_sequence;
            u32 consumed=cp.width,units=cp.codepoint>0xffff?2u:1u;
            const auto next=at+consumed<input.bytes?utf8_decode_one(input.data+at+consumed,input.bytes-at-consumed):Utf8DecodeResult{};
            const bool selector=next.valid&&unicode_properties(next.codepoint)->has(UnicodeFlag::variation_selector);
            if(selector){consumed+=next.width;units+=next.codepoint>0xffff?2u:1u;}
            const bool pair=cp.codepoint==13&&at+consumed<input.bytes&&input.data[at+consumed]=='\n';
            if(shaped.cluster_count==s.clusters.count||units>s.source_glyphs.count-shaped.source_units)return SfntStatus::output_too_small;
            const bool newline=cp.codepoint-10u<4||cp.codepoint==0x85||cp.codepoint==0x2028||cp.codepoint==0x2029;
            const u32 flags=cp.codepoint==9?native_paragraph_tab:pair?native_paragraph_crlf_prefix:newline?native_paragraph_newline|(selector?native_paragraph_atomic_newline:0):native_paragraph_nonvisual;
            s.clusters[shaped.cluster_count++]={shaped.source_units,shaped.source_units+units,shaped.glyph_count,0,0,flags};
            for(u32 i=0;i<units;++i)s.source_glyphs[shaped.source_units+i]=0xffffffffu;
            shaped.source_units+=units;at+=consumed;continue;
        }
        u32 end=at+cp.width;while(end<input.bytes){const auto next=utf8_decode_one(input.data+end,input.bytes-end);if(!next.valid)return SfntStatus::invalid_input;
            if(native_text_nonvisual(next.codepoint))break;end+=next.width;}
        NativeAsciiLineResult line{};status=native_emoji_line(store,plan,{input.data+at,end-at},em,s.work,&line,{},coverage,rtl,arabic,true);if(status!=SfntStatus::ok)return status;
        if(!(line.geometry_flags&native_line_ordered_x))return SfntStatus::unsupported_sequence;
        if(line.run.glyph_count>s.glyphs.count-shaped.glyph_count||line.run.glyph_count>s.placements.count-shaped.glyph_count||
           line.run.source_units>s.source_glyphs.count-shaped.source_units||line.cluster_count>s.clusters.count-shaped.cluster_count)return SfntStatus::output_too_small;
        for(u32 i=0;i<line.run.glyph_count;++i){s.glyphs[shaped.glyph_count+i]=s.work.run.glyphs[i];s.placements[shaped.glyph_count+i]=s.work.run.placements[i];}
        for(u32 i=0;i<line.run.source_units;++i)s.source_glyphs[shaped.source_units+i]=shaped.glyph_count+s.work.run.character_glyphs[i];
        u32 first=0;for(u32 i=0;i<line.cluster_count;++i){const auto& c=s.work.clusters[i];const u32 g=s.work.run.character_glyphs[first],ge=c.source_end==line.run.source_units?line.run.glyph_count:s.work.run.character_glyphs[c.source_end];
            s.clusters[shaped.cluster_count++]={shaped.source_units+first,shaped.source_units+c.source_end,shaped.glyph_count+g,ge-g,c.width,0};first=c.source_end;}
        shaped.source_units+=line.run.source_units;shaped.glyph_count+=line.run.glyph_count;at=end;
    }
    if(!boundaries.deferred){status=native_latin_paragraph_boundaries(input,{s.clusters.data,shaped.cluster_count},boundaries);if(status!=SfntStatus::ok)return status;}
    *output=shaped;return SfntStatus::ok;
}
}
