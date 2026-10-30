#include "maicord/text/emoji_paragraph.hpp"
#include "unicode_line_geometry.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/visibility.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/text/base.hpp"
namespace maicord {
SfntStatus native_emoji_paragraph_shape(const NativeRunPlanStore& store,const NativeEmojiPlan& plan,Utf8View input,float em,
    const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,bool coverage,bool rtl,bool arabic) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(input.bytes>65535)return SfntStatus::resource_limit;
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
            const bool newline=cp.codepoint-10u<4||cp.codepoint==0x85||cp.codepoint==0x2028||cp.codepoint==0x2029;
            const u32 flags=cp.codepoint==9?native_paragraph_tab:pair?native_paragraph_crlf_prefix:newline?native_paragraph_newline|(selector?native_paragraph_atomic_newline:0):native_paragraph_nonvisual;
            status=native_paragraph_append_control(s,shaped,units,flags);if(status!=SfntStatus::ok)return status;
            at+=consumed;continue;
        }
        u32 end=at+cp.width;while(end<input.bytes){const auto next=utf8_decode_one(input.data+end,input.bytes-end);if(!next.valid)return SfntStatus::invalid_input;
            if(native_text_nonvisual(next.codepoint))break;end+=next.width;}
        NativeAsciiLineResult line{};status=native_emoji_line(store,plan,{input.data+at,end-at},em,s.work,&line,{},coverage,rtl,arabic,true);if(status!=SfntStatus::ok)return status;
        status=native_paragraph_append_line(s,line,shaped);if(status!=SfntStatus::ok)return status;
        at=end;
    }
    if(!boundaries.deferred){status=native_latin_paragraph_boundaries(input,{s.clusters.data,shaped.cluster_count},boundaries);if(status!=SfntStatus::ok)return status;}
    *output=shaped;return SfntStatus::ok;
}
}
