#include "maicord/text/latin_paragraph.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/latin_line.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/text/visibility.hpp"
#include "maicord/text/base.hpp"
#include "maicord/text/detail/line_break_tailored.hpp"
#include "maicord/text/detail/thai_word_break.hpp"
#include "script_paragraph.hpp"
namespace maicord {namespace {
bool control(u32 cp) noexcept {return cp==9||cp==10||cp==13||cp==0x85||cp==0x2028||cp==0x2029;}
bool nonvisual(u32 cp) noexcept {return cp==0x200b||native_text_bidi_format(cp)||native_text_zero_width_control(cp);}
void format_boundaries(ConstSlice<u32> scalars,Slice<u8> breaks) noexcept {
    // Windows retains explicit-level cuts between invisible items even when
    // their final resolved levels agree. Reuse a free bit in boundary scratch.
    u8 levels[126]{};u32 depth=1,overflow=0;u8 previous=0;bool previous_format=false;
    for(u32 i=0;i<scalars.count;++i){const u32 cp=scalars[i];u8 level=levels[depth-1];
        if(cp==0x202a||cp==0x202b||cp==0x202d||cp==0x202e){const bool odd=cp==0x202b||cp==0x202e;
            const u32 next=odd?((level+1u)|1u):((level+2u)&~1u);
            if(next<=125&&!overflow){level=static_cast<u8>(next);levels[depth++]=level;}else ++overflow;
        }else if(cp==0x202c){if(overflow)--overflow;else if(depth>1)--depth;}
        const bool format=native_text_bidi_format(cp);if(format&&previous_format&&level!=previous)breaks[i]|=4;
        previous=level;previous_format=format;
        if(unicode_properties(cp)->bidi_class()==UnicodeBidiClass::b){depth=1;overflow=0;previous=0;previous_format=false;}
    }
}
}
SfntStatus native_latin_paragraph_boundaries(Utf8View text,Slice<NativeParagraphCluster> clusters,NativeLatinBoundaryStorage s) noexcept {
    if((text.bytes&&!text.data)||(clusters.count&&!clusters.data)||(s.scalar_count&&!s.scalar_data)||(s.breaks.count&&!s.breaks.data))return SfntStatus::invalid_input;
    u32 count=0,units=0;bool thai=false,bidi_format=false;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return SfntStatus::invalid_input;
        if(count==s.scalar_count)return SfntStatus::output_too_small;s.scalar_data[count++]=cp.codepoint;thai|=cp.codepoint-0xe00u<128;bidi_format|=native_text_bidi_format(cp.codepoint);units+=cp.codepoint>65535?2u:1u;at+=cp.width;}
    if(s.breaks.count<=count)return SfntStatus::output_too_small;
    if(!line_breaks_tailored({s.scalar_data,count},s.breaks))return SfntStatus::invalid_input;
    if(thai&&!native_thai_word_breaks({s.scalar_data,count},s.breaks))return SfntStatus::invalid_input;
    if(bidi_format)format_boundaries({s.scalar_data,count},s.breaks);
    u32 scalar=0,source=0,previous=0;
    for(u32 i=0;i<clusters.count;++i){auto& c=clusters[i];
        if(c.source_first!=previous||c.source_end<=c.source_first||c.source_end>units)return SfntStatus::invalid_input;
        while(scalar<count&&source<c.source_first){source+=s.scalar_data[scalar++]>65535?2u:1u;}
        if(source!=c.source_first||scalar==count)return SfntStatus::unsupported_sequence;
        // Separate font/effect items may split CRLF while retaining distinct
        // source ranges. Its CR is a prefix, not a second mandatory break.
        if(s.scalar_data[scalar]==13&&scalar+1<count&&s.scalar_data[scalar+1]==10&&c.source_end==c.source_first+1&&
           !c.glyph_count&&(c.flags&native_paragraph_newline)&&i+1<clusters.count&&clusters[i+1].source_first==c.source_end&&
           !clusters[i+1].glyph_count&&(clusters[i+1].flags&native_paragraph_newline))
            c.flags=(c.flags&~native_paragraph_newline)|native_paragraph_crlf_prefix;
        c.flags&=~(native_paragraph_break_before|native_paragraph_space);
        if(i&&(s.breaks[scalar]&3))c.flags|=native_paragraph_break_before;
        if(bidi_format&&(c.flags&native_paragraph_nonvisual)){c.flags&=~native_paragraph_metric_group;if(s.breaks[scalar]&4)c.flags|=native_paragraph_metric_group;}
        const auto& property=*unicode_properties(s.scalar_data[scalar]);
        // Source analysis survives font/effect cuts. One selector following a
        // space retains its whitespace classification even if font mapping
        // leaves the selector in a separate, nonempty missing-glyph cluster.
        const bool space=property.has(UnicodeFlag::white_space)||(scalar&&property.has(UnicodeFlag::variation_selector)&&
            !control(s.scalar_data[scalar-1])&&unicode_properties(s.scalar_data[scalar-1])->has(UnicodeFlag::white_space));
        if(!(c.flags&(native_paragraph_tab|native_paragraph_newline|native_paragraph_crlf_prefix))&&space)c.flags|=native_paragraph_space;
        previous=c.source_end;
    }
    return previous==units?SfntStatus::ok:SfntStatus::invalid_input;
}
SfntStatus native_script_paragraph_shape(const SfntFontLayoutMetrics& metrics,Utf8View input,float em,
    const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,NativeParagraphLineSource source,bool* nominal) noexcept {
    if(nominal)*nominal=false;
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(input.bytes>65535)return SfntStatus::resource_limit;
    if((input.bytes&&!input.data)||(s.source_glyphs.count&&!s.source_glyphs.data)||(s.clusters.count&&!s.clusters.data)||
       (s.glyphs.count&&!s.glyphs.data)||(s.placements.count&&!s.placements.data))return SfntStatus::invalid_input;
    NativeParagraphShape shaped{};bool simple=true;if(!source.shape)return SfntStatus::invalid_input;
    auto status=sfnt_single_font_line(metrics,em,&shaped.vertical);if(status!=SfntStatus::ok)return status;
    for(u32 at=0;at<input.bytes;){const auto cp=utf8_decode_one(input.data+at,input.bytes-at);if(!cp.valid)return SfntStatus::invalid_input;
        if(control(cp.codepoint)||nonvisual(cp.codepoint)){
            simple=false;
            const u32 count=cp.codepoint==13&&at+1<input.bytes&&input.data[at+1]=='\n'?2u:1u;
            status=native_paragraph_append_control(s,shaped,count,nonvisual(cp.codepoint)?native_paragraph_nonvisual:cp.codepoint==9?native_paragraph_tab:native_paragraph_newline);if(status!=SfntStatus::ok)return status;
            at+=cp.width+(count-1);continue;
        }
        u32 end=at+cp.width;while(end<input.bytes){const auto next=utf8_decode_one(input.data+end,input.bytes-end);if(!next.valid)return SfntStatus::invalid_input;if(control(next.codepoint)||nonvisual(next.codepoint))break;end+=next.width;}
        NativeAsciiLineResult line{};status=source.shape(source.context,{input.data+at,end-at},em,s.work,&line);if(status!=SfntStatus::ok)return status;
        simple=simple&&line.run.nominal_ranges!=0;
        status=native_paragraph_append_line(s,line,shaped);if(status!=SfntStatus::ok)return status;
        at=end;
    }
    if(!boundaries.deferred){status=native_latin_paragraph_boundaries(input,{s.clusters.data,shaped.cluster_count},boundaries);if(status!=SfntStatus::ok)return status;}
    *output=shaped;if(nominal)*nominal=simple;return SfntStatus::ok;
}
SfntStatus native_latin_paragraph_shape(const NativeRunPlanStore& store,NativeRunPlanHandle plan,Utf8View input,bool independent,float em,
    const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,bool* nominal,float trailing_spacing,const NativeAlphabeticPlan* profile,bool force_shaping,bool rtl) noexcept {
    if(output)reset_value(output);if(nominal)*nominal=false;
    const auto* metrics=native_run_plan_metrics(store,plan);if(!metrics)return SfntStatus::invalid_input;
    struct Source {const NativeRunPlanStore& store;NativeRunPlanHandle plan;const NativeAlphabeticPlan* profile;float spacing;bool independent,force,rtl;} source{store,plan,profile,trailing_spacing,independent,force_shaping,rtl};
    const auto shape=[](void* opaque,Utf8View text,float size,const NativeAsciiLineStorage& storage,NativeAsciiLineResult* result) noexcept {
        const auto& c=*static_cast<Source*>(opaque);return native_latin_line(c.store,c.plan,text,c.independent,size,storage,result,{},c.spacing,c.profile,c.force,c.rtl,true);
    };
    return native_script_paragraph_shape(*metrics,input,em,s,boundaries,output,{&source,shape},nominal);
}

}
