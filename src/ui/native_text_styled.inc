#include "native_text_decorations.inc"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/crt.hpp"
#include "maicord/sys/simd.hpp"
namespace {
bool old_hangul_jamo(u32 cp) noexcept {
    if(cp<0x1113)return false;
    return (cp>=0x1113&&cp<=0x1160)||(cp>=0x1176&&cp<=0x11a7)||(cp>=0x11c3&&cp<=0x11ff)||
        (cp>=0xa960&&cp<=0xa97c)||(cp>=0xd7b0&&cp<=0xd7c6)||(cp>=0xd7cb&&cp<=0xd7fb);
}
bool directional_scripts(NativeTextContext& owner,Utf8View text,u8* scripts,u8* levels) noexcept {
    arena_reset(&owner.scratch);auto scalars=take<u32>(owner.scratch,text.bytes);auto classes=take<NativeScriptClass>(owner.scratch,text.bytes);
    auto cells=take<UnicodeBidiCell>(owner.scratch,text.bytes);
    if(!scalars.data||!classes.data||!cells.data)return false;u32 count=0;bool tabs=false,old_hangul=false,plain_ltr=true;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return false;tabs|=cp.codepoint==9;old_hangul|=old_hangul_jamo(cp.codepoint);
        const auto type=unicode_properties(cp.codepoint)->bidi_class();plain_ltr=plain_ltr&&native_text_ltr_class(type);
        cells[count]={cp.codepoint,~0u,~0u,type,type,0,0};scalars[count++]=cp.codepoint;at+=cp.width;}
    if(native_script_resolve({scalars.data,count},classes)!=NativeScriptStatus::ok)return false;
    if(old_hangul){UnicodeGraphemeState state{};u32 first=0;bool old=false;
        for(u32 i=0;i<=count;++i){bool boundary=i==count;if(i<count&&!unicode_grapheme_next(&state,scalars[i],&boundary))return false;
            if(boundary){if(old)for(u32 j=first;j<i;++j)if(scalars[j]-0xac00u>=11172)classes[j].flags|=128;first=i;old=false;}
            if(i<count)old|=old_hangul_jamo(scalars[i]);}}
    if(plain_ltr){for(u32 i=0;i<count;++i)levels[i]=0;}
    else {
    auto order=take<u32>(owner.scratch,text.bytes);if(!order.data)return false;
    // In LTR paragraphs a tab supplies L context, including within bracket
    // pairs, so pairs stay intact. S is restored before the standard L1 reset.
    if(tabs)for(u32 i=0;i<count;++i)if(scalars[i]==9)cells[i].original=UnicodeBidiClass::l;
    for(u32 first=0;first<count;){u32 end=0;UnicodeBidiParagraph paragraph{};
        if(unicode_bidi_paragraph_end({cells.data,count},first,&end)!=UnicodeBidiStatus::ok||end<=first)return false;
        if(bidi_resolve_tailored({cells.data+first,end-first},UnicodeParagraphDirection::ltr,order,&paragraph)!=UnicodeBidiStatus::ok)return false;
        if(tabs)for(u32 i=first;i<end;++i)if(scalars[i]==9)cells[i].original=UnicodeBidiClass::s;
        // Windows keeps explicit embedding/override source controls in its
        // whitespace-boundary scan, although those controls draw no glyphs.
        u8 preceding=0;for(u32 i=first;i<end;++i){auto& cell=cells[i];
            if(cell.level==unicode_bidi_removed){if(scalars[i]>=0x202a&&scalars[i]<=0x202e)cell.level=preceding;}
            else preceding=cell.level;}
        u32 visual=0;if(unicode_bidi_line({cells.data+first,end-first},0,0,end-first,{levels+first,end-first},{},&visual)!=UnicodeBidiStatus::ok)return false;first=end;}
    u8 previous=0;for(u32 i=0;i<count;++i){if(levels[i]==unicode_bidi_removed)levels[i]=previous;else previous=levels[i];}
    }
    // Expand scalar metadata backward into UTF-8 bytes without another array.
    u32 at=text.bytes;u16 previous_script=256;u8 profile=0;
    for(u32 index=count;index;){--index;const auto script=classes[index].script;
        if(static_cast<u8>(script)!=previous_script){profile=native_text_script_profile(script,true);if(!profile)return false;previous_script=static_cast<u8>(script);}
        const u8 value=(classes[index].flags&128)?static_cast<u8>(old_hangul_script>>3):profile;
        const u32 scalar=scalars[index],width=scalar<0x80?1u:scalar<0x800?2u:scalar<0x10000?3u:4u;const u8 level=levels[index];at-=width;
        for(u32 i=0;i<width;++i){scripts[at+i]=value;levels[at+i]=level;}
    }return true;
}
ConstSlice<u8> prepare_line_reset(NativeTextContext& owner) noexcept {
    const auto& s=owner.paragraph_storage;const u32 count=owner.paragraph_shape.cluster_count;
    // Run reshaping leaves the single-line cluster scratch unused, so it holds
    // L1 eligibility across fitting and publication. Highly fragmented fallback
    // items may not fit and take a transient arena suffix instead.
    const auto flags=static_cast<u64>(s.work.clusters.count)*sizeof(NativeLineCluster)>=count?
        Slice<u8>{reinterpret_cast<u8*>(s.work.clusters.data),count}:take<u8>(owner.scratch,count);
    if(flags.count<count||!flags.data)return {};u32 byte=0,source=0;
    for(u32 i=0;i<count;++i){while(source<s.clusters[i].source_first){const auto cp=utf8_decode_one(owner.last_text.data+byte,owner.last_text.count-byte);if(!cp.valid)return {};source+=cp.codepoint>65535?2u:1u;byte+=cp.width;}
        const auto cp=utf8_decode_one(owner.last_text.data+byte,owner.last_text.count-byte);if(!cp.valid)return {};
        flags[i]=static_cast<u8>((s.clusters[i].flags&native_paragraph_space)?1:unicode_properties(cp.codepoint)->bidi_class()==UnicodeBidiClass::bn?2:0);
    }
    bool suffix=true;for(u32 i=count;i;){--i;suffix=suffix&&flags[i]!=0;if(flags[i]==1&&suffix)flags[i]=3;}
    bool previous=false;for(u32 i=0;i<count;++i){const u8 value=flags[i];if(value==2)flags[i]=previous?1:0;else{previous=value==3;flags[i]=value?1:0;}}
    return {flags.data,count};
}
struct RtlLineOutput {NativeTextContext& owner;ShapedText* output;u32 written=0,byte=0,source=0,span=0;bool omission=false;u8 omission_level=0;float omission_width=0,omission_x=0;u32 omission_visual=0;Slice<NativeOmissionZero> zeroes{};bool source_gaps=false;u32 omission_before=0xffffffffu;};
bool split_rtl_fallback_space(void* context,u32 style) noexcept {
    const auto& owner=static_cast<RtlLineOutput*>(context)->owner;
    if(!(owner.last_mode&32)||!style||style>=owner.styled_spans.count||(owner.styled_analysis[style]&1))return false;
    if(((owner.styled_analysis[style-1]>>8)<<3)!=arabic_script||((owner.styled_analysis[style]>>8)<<3)==arabic_script)return false;
    // The Arabic trailing space can return to the requested face before the
    // following non-Arabic run. A font change within Arabic punctuation keeps
    // its leading space in the same drawing group.
    const auto a=owner.styled_spans[style-1].binding()->font,b=owner.styled_spans[style].binding()->font;
    return a.slot!=b.slot||a.generation!=b.generation;
}
SfntStatus publish_rtl_line(void* context,ConstSlice<NativeParagraphRun> runs,u32 first,u32 trailing_source) noexcept {
    auto& state=*static_cast<RtlLineOutput*>(context);auto& owner=state.owner;auto& output=*state.output;const auto& s=owner.paragraph_storage;
    if(first>output.run_capacity||runs.count>output.run_capacity-first||(runs.count&&!output.runs)||!output.glyphs||!output.clusters){output_capacity_failure(owner);return SfntStatus::output_too_small;}
    if(!runs.count&&!state.omission)return SfntStatus::ok;
    // Trimming keeps the original contextual glyph forms. An omission is an
    // inline object within the shaped line, not a new shaping boundary.
    const bool refit=!state.omission&&owner.last_independent&&runs.count&&(first||static_cast<u32>(runs[runs.count-1].source_first)+runs[runs.count-1].source_count<owner.paragraph_shape.source_units);
    u32 trailing=0;while(trailing<runs.count&&runs[trailing].source_first<trailing_source)++trailing;
    if(trailing&&static_cast<u32>(runs[trailing-1].source_first)+runs[trailing-1].source_count>trailing_source)return SfntStatus::unsupported_sequence;
    const bool mixed=(owner.last_mode&32)!=0;
    const auto analysis_for=[&](u32 span) noexcept -> u32 {return mixed?((static_cast<u32>(owner.styled_analysis[span]&0xff00u)<<3)|(static_cast<u32>(owner.styled_spans[span].shaping_flags())<<8)|(owner.styled_analysis[span]&255u)):((64u<<8)|1u);};
    const auto level_for=[&](u32 run,u32 analysis) noexcept {return run>=trailing?static_cast<u8>(0):static_cast<u8>(analysis);};
    for(u32 run=0;run<runs.count;){const auto& first_run=runs[run];
        while(state.span+1<owner.styled_spans.count&&owner.styled_spans[state.span].end_unit<=first_run.source_first)++state.span;
        const auto& item=owner.styled_spans[state.span];const u32 analysis=analysis_for(state.span);
        const bool emoji_item=item.binding()->font_flags==emoji_font_flag;
        const u8 level=level_for(run,analysis);const bool reshape=first_run.glyph_count&&refit&&
            !(item.flags & text_style_custom_emoji) && (!emoji_item||((analysis>>8)&~7u)==arabic_script);
        u32 last=run+1;
        // A drawing-effect boundary is not a shaping boundary. Refit adjacent
        // same-font/script/direction runs together, then restore paint ranges.
        if(reshape&&refit){u32 span=state.span;
            while(last<runs.count&&runs[last].glyph_count){
                while(span+1<owner.styled_spans.count&&owner.styled_spans[span].end_unit<=runs[last].source_first)++span;
                const auto& next=owner.styled_spans[span];const u32 other=analysis_for(span);
                if(((next.flags ^ item.flags) & text_style_custom_emoji)||next.em!=item.em||next.binding()->plan.slot!=item.binding()->plan.slot||next.binding()->plan.generation!=item.binding()->plan.generation||
                   ((next.binding()->key^item.binding()->key)&0x0c000000u)||((other^analysis)&0x7f901u)||((level_for(last,other)^level)&1)||(((analysis>>8)&~7u)==arabic_script&&level_for(last,other)!=level))break;
                ++last;
            }
        }
        if(state.source_gaps)while(state.source<first_run.source_first){const auto cp=utf8_decode_one(owner.last_text.data+state.byte,owner.last_text.count-state.byte);if(!cp.valid)return SfntStatus::invalid_input;state.source+=cp.codepoint>65535?2u:1u;state.byte+=cp.width;}
        if(state.source!=first_run.source_first)return SfntStatus::unsupported_sequence;
        const u32 first_byte=state.byte,first_source=state.source,end_source=static_cast<u32>(runs[last-1].source_first)+runs[last-1].source_count;
        while(state.source<end_source){const auto cp=utf8_decode_one(owner.last_text.data+state.byte,owner.last_text.count-state.byte);if(!cp.valid)return SfntStatus::invalid_input;state.source+=cp.codepoint>65535?2u:1u;state.byte+=cp.width;}
        if(state.source!=end_source)return SfntStatus::unsupported_sequence;NativeAsciiResult result{};
        if(reshape){const bool arabic=((analysis>>8)&~7u)==arabic_script,deva=((analysis>>8)&~7u)==devanagari_script,tamil=((analysis>>8)&~7u)==tamil_script,sinhala=((analysis>>8)&~7u)==sinhala_script,khmer=((analysis>>8)&~7u)==khmer_script,myanmar=((analysis>>8)&~7u)==myanmar_script,tibetan=((analysis>>8)&~7u)==tibetan_script,form=form_script(static_cast<u16>((analysis>>8)&~7u));const auto* profile=arabic||deva||tamil||sinhala||khmer||myanmar||tibetan||form||emoji_item?nullptr:alphabetic_profile(owner,*item.binding(),static_cast<u16>((analysis>>8)&~7u),owner.last_independent);if(!arabic&&!deva&&!tamil&&!sinhala&&!khmer&&!myanmar&&!tibetan&&!form&&!emoji_item&&!profile)return SfntStatus::unsupported_sequence;
            const Utf8View input{owner.last_text.data+first_byte,state.byte-first_byte};
            const float spacing=owner.styled_spans[0].em*owner.last_tracking;
            // L1 resets the published level of trailing whitespace. Spacing
            // and glyph positioning retain the item's analyzed direction.
            const bool shaping_rtl=(analysis&1)!=0;
            auto status=emoji_item?native_emoji_run(owner.store,*item.binding()->emoji,input,item.em,s.work.run,&result,{},false,shaping_rtl,arabic):form?form_run(form_script_profile(static_cast<u16>((analysis>>8)&~7u)),owner,*item.binding(),input,item.em,s.work.run,&result,spacing,shaping_rtl):tibetan?tibetan_run(owner,*item.binding(),input,item.em,s.work.run,&result,spacing,shaping_rtl):myanmar?myanmar_run(owner,*item.binding(),input,item.em,s.work.run,&result,spacing,shaping_rtl):khmer?khmer_run(owner,*item.binding(),input,item.em,s.work.run,&result,spacing,shaping_rtl):sinhala?sinhala_run(owner,*item.binding(),input,item.em,s.work.run,&result,spacing,shaping_rtl):tamil?tamil_run(owner,*item.binding(),input,item.em,s.work.run,&result,spacing,shaping_rtl):deva?devanagari_run(owner,*item.binding(),input,item.em,s.work.run,&result,spacing,shaping_rtl):arabic?native_arabic_run(owner.arabic_store,item.binding()->arabic,input,owner.last_independent,shaping_rtl,item.em,s.work.run,&result):
                native_alphabetic_run(owner.store,*profile,input,item.em,s.work.run,&result,{},profile->script_tag!=0&&!profile->allow_missing,!mixed||!(analysis&256),shaping_rtl);
            if(status==SfntStatus::ok&&spacing&&!deva&&!tamil&&!sinhala&&!khmer&&!myanmar&&!tibetan&&!form&&!emoji_item)status=sfnt_trailing_spacing({s.work.run.placements.data,result.glyph_count},
                result.nominal_ranges?ConstSlice<u8>{}:ConstSlice<u8>{s.work.run.shaping.flags.data,result.glyph_count},spacing,1000000,shaping_rtl);
            if(status!=SfntStatus::ok)return status;if(result.source_units!=end_source-first_source)return SfntStatus::unsupported_sequence;}
        for(u32 part=run;part<last;++part){const auto& r=runs[part];
            while(state.span+1<owner.styled_spans.count&&owner.styled_spans[state.span].end_unit<=r.source_first)++state.span;
            const u32 source=r.source_first-first_source,source_end=source+r.source_count;
            const u32 glyph_first=reshape?s.work.run.character_glyphs[source]:r.glyph_first;
            const u32 glyph_end=reshape?(source_end==result.source_units?result.glyph_count:s.work.run.character_glyphs[source_end]):r.glyph_first+r.glyph_count;
            if(glyph_end<glyph_first||(reshape&&glyph_end>result.glyph_count))return SfntStatus::unsupported_sequence;const u32 count=glyph_end-glyph_first;
            if(count>output.glyph_capacity-state.written||state.written+count>65535||end_source>output.cluster_capacity){output_capacity_failure(owner);return SfntStatus::output_too_small;}
            float width=r.glyph_count?0:r.width;
            for(u32 g=0;g<count;++g){const auto& placement=reshape?s.work.run.placements[glyph_first+g]:s.placements[r.glyph_first+g];const u16 glyph=reshape?s.work.run.glyphs[glyph_first+g]:s.glyphs[r.glyph_first+g];
                const float advance=refit?placement.advance:s.placements[r.glyph_first+g].advance;width=decoration_add(width,advance);
                output.glyphs[state.written+g]={advance,placement.advance_offset,placement.ascender_offset,glyph,0};}
            for(u32 u=0;u<r.source_count;++u){const u32 glyph=reshape?s.work.run.character_glyphs[source+u]:s.source_glyphs[r.source_first+u];
                output.clusters[r.source_first+u]=glyph==0xffffffffu?0xffffu:static_cast<u16>(state.written+glyph-glyph_first);}
            auto& target=output.runs[first+part];target.first_glyph=state.written;target.glyph_count=count;target.baseline_x=width;
            target.bidi_level=level_for(part,analysis_for(state.span));state.written+=count;
        }run=last;
    }
    // Widths temporarily occupy the unpublished origin fields. Reorder only
    // after line-specific shaping, whose advances can differ from fit metrics.
    if((owner.last_mode&32)||state.omission){const auto mark=arena_mark(&owner.scratch);const u32 count=runs.count+(state.omission?1u+state.zeroes.count:0u);auto order=take<u32>(owner.scratch,count);
        if(!order.data)return SfntStatus::output_too_small;u32 highest=0,lowest=255;
        const auto level_for_item=[&](u32 i) noexcept {return i>=runs.count?(i==runs.count?state.omission_level:state.zeroes[i-runs.count-1].level):output.runs[first+i].bidi_level;};
        const u32 before=minimum(state.omission_before,runs.count),objects=state.omission?1u+state.zeroes.count:0u;
        for(u32 i=0;i<count;++i){order[i]=i<before?i:i<before+objects?runs.count+i-before:i-objects;const u32 level=level_for_item(i);highest=maximum(highest,level);if(level&1)lowest=minimum(lowest,level);}
        for(u32 level=highest;level>=lowest;--level)for(u32 i=0;i<count;){
            if(level_for_item(order[i])<level){++i;continue;}const u32 begin=i;while(i<count&&level_for_item(order[i])>=level)++i;
            for(u32 a=begin,b=i-1;a<b;++a,--b){const u32 value=order[a];order[a]=order[b];order[b]=value;}}
        // The unpublished Y field carries the visual left edge for decorations.
        // Subtracting width from an RTL right origin can lose one float step.
        float pen=0;u32 visual=0;for(u32 i=0;i<count;++i){if(order[i]>=runs.count){if(order[i]==runs.count){state.omission_x=pen;state.omission_visual=visual;pen=decoration_add(pen,state.omission_width);}else state.zeroes[order[i]-runs.count-1].x=pen;continue;}auto& target=output.runs[first+order[i]];const float width=target.baseline_x;target.baseline_y=pen;target.text_position=visual++;
            if(target.bidi_level&1){pen=decoration_add(pen,width);target.baseline_x=pen;}else{target.baseline_x=pen;pen=decoration_add(pen,width);}}
        arena_rewind(&owner.scratch,mark);return SfntStatus::ok;
    }
    float pen=0;for(u32 i=trailing;i;){--i;auto& target=output.runs[first+i];target.baseline_y=pen;target.text_position=trailing-1-i;pen=decoration_add(pen,target.baseline_x);target.baseline_x=pen;target.bidi_level=1;}
    for(u32 i=trailing;i<runs.count;++i){auto& target=output.runs[first+i];const float width=target.baseline_x;target.baseline_y=pen;target.text_position=i;target.baseline_x=pen;target.bidi_level=0;pen=decoration_add(pen,width);}
    return SfntStatus::ok;
}
#include "native_text_directional_hits.inc"
bool alphabetic_scripts(NativeTextContext& owner,Utf8View text,u8* bytes) noexcept {
    arena_reset(&owner.scratch);auto scalars=take<u32>(owner.scratch,text.bytes);auto classes=take<NativeScriptClass>(owner.scratch,text.bytes);
    if(!scalars.data||!classes.data)return false;u32 count=0;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return false;scalars[count++]=cp.codepoint;at+=cp.width;}
    if(native_script_resolve({scalars.data,count},classes)!=NativeScriptStatus::ok)return false;
    u32 index=0;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);const auto value=classes[index++];
        if(value.script!=UnicodeScript::latn&&value.script!=UnicodeScript::grek&&value.script!=UnicodeScript::cyrl&&value.script!=UnicodeScript::zyyy&&value.script!=UnicodeScript::zzzz)return false;
        const u8 script=value.script==UnicodeScript::grek?16u:value.script==UnicodeScript::cyrl?32u:0u;
        for(u32 i=0;i<cp.width;++i)bytes[at+i]=static_cast<u8>(script>>3);at+=cp.width;}
    return true;
}
// Intersects paint/font styles with the maximal emoji font overrides. One sweep
// counts the exact storage and fills it; script/bidi resolution is separate.
u32 requested_font(const TextSystem& system,u32 flags) noexcept {u32 result=flags&(text_style_bold|text_style_italic|text_style_monospace);if(text_system_fixed_weight(system,flags))result&=~text_style_bold;return result;}
float span_size(const TextSystem& system,const NativeFormat* format,u32 flags) noexcept {const float size=text_system_font_size(system,flags);return size!=system.em_size?size:format->em;}
bool same_query(const NativeTextFontQuery& a,const NativeTextFontQuery& b) noexcept {return a.scope==b.scope&&bytes_equal(&a.properties,&b.properties,4)&&a.family.count==b.family.count&&
    CompareStringOrdinal(reinterpret_cast<const wchar_t*>(a.family.data),static_cast<int>(a.family.count),reinterpret_cast<const wchar_t*>(b.family.data),static_cast<int>(b.family.count),TRUE)==CSTR_EQUAL;}
u32 font_span_end(ConstSlice<TextStyleSpan> styles,u32 at,u32 limit,u32 style,const TextSystem& system,const NativeFormat* format,u32 flags,bool decorations=true) noexcept {
    const auto query=font_query(*system.native_text,format,requested_font(system,flags));const float em=span_size(system,format,flags);
    while(at<limit){while(style<styles.count&&styles[style].byte_offset+styles[style].byte_count<=at)++style;u32 end=limit,value=0;
        if(style<styles.count){const auto& s=styles[style];if(s.byte_offset<=at){value=s.flags;end=minimum(end,s.byte_offset+s.byte_count);}else end=minimum(end,s.byte_offset);}
        if(span_size(system,format,value)!=em||(decorations&&((value^flags)&(text_style_underline|text_style_strikethrough)))||!same_query(query,font_query(*system.native_text,format,requested_font(system,value))))break;if(end<=at)return 0;at=end;
    }return at;
}
bool layout_control(u32 cp) noexcept {return cp==9||cp==10||cp==13||cp==0x85||cp==0x2028||cp==0x2029||cp==0x200b||native_text_bidi_format(cp)||native_text_zero_width_control(cp);}
u32 presentation_spans(Utf8View text,ConstSlice<TextStyleSpan> styles,u8 presentation,StyledSpan* spans=nullptr,u8* continuity=nullptr,
    const TextSystem* system=nullptr,const NativeFormat* format=nullptr,const u8* scripts=nullptr,bool fallback=false,bool* changed=nullptr,bool editor=false,u8* directions=nullptr,
    Slice<StyledSpan> prepared={}) noexcept {
    u32 previous=0;
    for(u32 i=0;i<styles.count;++i){const auto& s=styles[i];
        if(!s.flags||!s.byte_count||s.byte_offset<previous||s.byte_offset>text.bytes||s.byte_count>text.bytes-s.byte_offset)return 0;
        previous=s.byte_offset+s.byte_count;}
    // 0: ordinary, 1: the validated override covers the whole input, 2: mixed.
    // Reuse the caller's whole-input result instead of rescanning every emoji.
    TextEmojiCursor cursor{};TextEmojiRange range{};auto next=presentation==2?text_emoji_next(text,&cursor,&range):TextEmojiNext::end;
    if(next==TextEmojiNext::invalid_input)return 0;
    u32 trailing=text.bytes;
    if(scripts&&!directions&&scripts[0]==8)while(trailing){
        if(text.data[trailing-1]==' '){--trailing;continue;}
        if(trailing>=3&&static_cast<u8>(text.data[trailing-3])==0xe2&&static_cast<u8>(text.data[trailing-2])==0x80&&
           (static_cast<u8>(text.data[trailing-1])==0x8c||static_cast<u8>(text.data[trailing-1])==0x8d)){trailing-=3;continue;}break;}
    u32 count=0,style=0,mapped_end=0,mapping_limit=0,script_end=0;Binding* mapped=nullptr;bool control=false;u16 mapped_script=0;
    for(u32 at=0;at<text.bytes;){
        while(style<styles.count&&styles[style].byte_offset+styles[style].byte_count<=at)++style;
        if(next==TextEmojiNext::range&&range.end_byte<=at){next=text_emoji_next(text,&cursor,&range);if(next==TextEmojiNext::invalid_input)return 0;}
        u32 end=text.bytes,flags=0;
        if(style<styles.count){const auto& s=styles[style];if(s.byte_offset<=at){flags=s.flags;end=s.byte_offset+s.byte_count;}else end=s.byte_offset;}
        const bool selected=presentation==1||(next==TextEmojiNext::range&&range.first_byte<=at);
        u32 item_end=text.bytes;if(next==TextEmojiNext::range)item_end=minimum(item_end,selected?range.end_byte:range.first_byte);
        // The emoji font override resets typography, not Arabic join-control
        // ownership. Keep that script fact even for an even-level emoji item.
        u16 script=scripts&&(!selected||static_cast<u16>(scripts[at]<<3)==arabic_script)?static_cast<u16>(scripts[at]<<3):0;
        if((scripts&&!selected)||directions){if(at>=script_end){script_end=at+1;while(script_end<item_end&&(selected||static_cast<u16>(scripts[script_end]<<3)==script)&&(!directions||directions[script_end]==directions[at]))++script_end;}item_end=minimum(item_end,script_end);}end=minimum(end,item_end);
        if(end<=at)return 0;
        Binding* font=nullptr;
        if(system){const u32 requested=requested_font(*system,flags);
            auto* base=binding(*system->native_text,format,selected?emoji_font_flag:requested);font=base;
            if(fallback&&!selected){if(!base)return 0;
                if(at>=mapped_end){
                    mapped_script=script;
                    if(at>=mapping_limit){mapping_limit=editor?font_span_end(styles,at,item_end,style,*system,format,flags):end;if(mapping_limit<=at)return 0;
                        const auto first=utf8_decode_one(text.data+at,mapping_limit-at);if(!first.valid)return 0;control=layout_control(first.codepoint);u32 previous_scalar=first.codepoint;
                        for(u32 byte=at+first.width;byte<mapping_limit;){const auto cp=utf8_decode_one(text.data+byte,mapping_limit-byte);if(!cp.valid)return 0;
                            // Hebrew layout ends the font-mapping item before a space
                            // following ZWJ or CGJ; otherwise routing's join affinity
                            // would keep the Hebrew fallback face for that space.
                            if(layout_control(cp.codepoint)!=control||(script==64&&(directions?!(directions[byte]&1):byte>=trailing)&&
                               (previous_scalar==0x200d||previous_scalar==0x34f)&&cp.codepoint==32)){mapping_limit=byte;break;}
                            previous_scalar=cp.codepoint;byte+=cp.width;}
                    }
                    if(control){mapped=base;mapped_end=mapping_limit;}
                    else{auto& owner=*system->native_text;NativeTextFontRange choice{};
                        if(!native_text_catalog_range(&owner.catalogs,font_query(owner,format,requested),owner.fonts,&owner.scratch,{text.data+at,mapping_limit-at},&choice))return 0;
                        if(!choice.bytes)return 0;
                        const bool missing=choice.match.candidate==native_fallback_missing;
                        mapped=missing?base:binding_face(owner,format,requested,choice);if(missing)mapped_script=script==egyptian_script?egyptian_missing_script:128;mapped_end=at+choice.bytes;
                        if(!missing&&directions&&(!(directions[at]&1)||script==arabic_script)&&script!=64){bool neutral=true,symbol=false;
                            for(u32 byte=at;neutral&&byte<mapped_end;){const auto cp=utf8_decode_one(text.data+byte,mapped_end-byte);if(!cp.valid)return 0;
                                neutral=(native_script_class(cp.codepoint).flags&native_script_contextual)!=0;const auto category=unicode_properties(cp.codepoint)->general_category();
                                symbol|=category==UnicodeGeneralCategory::sm||category==UnicodeGeneralCategory::sc||category==UnicodeGeneralCategory::sk||category==UnicodeGeneralCategory::so;byte+=cp.width;}
                            if(neutral&&symbol)mapped_script=128;
                        }}
                    // A paint cut does not detach a selector from a base-font
                    // glyph. A fallback face creates a real font boundary.
                    while((!editor||directions)&&!control&&mapped==base&&mapped_end==mapping_limit&&mapped_end<item_end){const u8 lead=static_cast<u8>(text.data[mapped_end]);
                        if(lead==0xe1||lead==0xef||lead==0xf3){const auto selector=utf8_decode_one(text.data+mapped_end,item_end-mapped_end);
                            if(selector.valid&&unicode_properties(selector.codepoint)->has(UnicodeFlag::variation_selector)&&
                               font_span_end(styles,mapped_end,mapped_end+selector.width,style,*system,format,flags,false)==mapped_end+selector.width){mapped_end+=selector.width;mapping_limit=mapped_end;continue;}}break;}
                }
                // Han fallback items retain independent advance reductions even
                // when adjacent items select the same physical face. Editor
                // paint cuts within one mapped font-property item do not.
                if(editor&&mapped_script==han_script)end=mapping_limit;
                font=mapped;script=mapped_script;end=minimum(end,mapped_end);if(changed&&font!=base)*changed=true;
                if(editor&&(font->key&binding_missing_family)&&(script==64||script==arabic_script||script==armenian_script||script==georgian_script)){
                    const bool standard=script==armenian_script||script==georgian_script;
                    // A missing family resolves neutral punctuation and spaces
                    // as one item. Marks and join controls keep Arabic context.
                    const auto kind=[&](u32 byte) noexcept {if(text.data[byte]==' ')return 1u;
                        if(script==arabic_script){const auto cp=utf8_decode_one(text.data+byte,end-byte);const auto category=unicode_properties(cp.codepoint)->general_category();
                            return category>=UnicodeGeneralCategory::pc&&category<=UnicodeGeneralCategory::ps&&(native_script_class(cp.codepoint).flags&native_script_contextual)?1u:0u;}
                        if(!standard)return 0u;
                        const auto cp=utf8_decode_one(text.data+byte,end-byte);return unicode_bidi_bracket(cp.codepoint).type!=UnicodeBracketType::none?2u:0u;};
                    const u32 item=kind(at);const bool space=item==1;u32 limit=at;
                    while(limit<end&&kind(limit)==item){const auto cp=utf8_decode_one(text.data+limit,end-limit);if(!cp.valid)return 0;limit+=cp.width;}
                    if(!space&&limit<end){u32 previous_byte=limit-1;while(previous_byte>at&&(static_cast<u8>(text.data[previous_byte])&0xc0)==0x80)--previous_byte;
                        const auto cp=utf8_decode_one(text.data+previous_byte,limit-previous_byte);
                        if(cp.valid&&(cp.codepoint==0x200d||cp.codepoint==0x34f))while(limit<end&&text.data[limit]==' ')++limit;
                    }
                    end=limit;if(item)script=128;
                }
            }
            if(!font||(selected&&!emoji_profile(*system->native_text,*font)))return 0;
            if(!selected&&!prepare_script_profile(*system->native_text,*font,script,system->independent_letters)){
                if(script!=arabic_script&&script!=devanagari_script&&script!=tamil_script&&script!=sinhala_script&&script!=khmer_script&&script!=myanmar_script&&script!=tibetan_script&&!form_script(static_cast<u16>(script)))return 0;
                // A fallback item can leave spaces in a base face with no
                // script program. Neutral characters use its Common frontend;
                // letters still require their script shaping profile.
                for(u32 byte=at;byte<end;){const auto cp=utf8_decode_one(text.data+byte,end-byte);if(!cp.valid||(!(native_script_class(cp.codepoint).flags&native_script_contextual)&&!layout_control(cp.codepoint)))return 0;byte+=cp.width;}
                script=128;if(!prepare_script_profile(*system->native_text,*font,script,system->independent_letters))return 0;
            }}
        // Windows layout publishes long font/effect items in chunks of at most
        // 16000 UTF-16 units; keep those accumulation boundaries.
        if(end-at>16000){u32 byte=at,units=0;while(byte<end){const auto cp=utf8_decode_one(text.data+byte,end-byte);if(!cp.valid)return 0;
            const u32 width=cp.codepoint>65535?2u:1u;if(units+width>16000)break;units+=width;byte+=cp.width;}end=byte;}
        // The byte script array may share continuity: this sweep only writes
        // its consumed prefix after finding the next source boundary.
        if(spans||count<prepared.count){auto& item=spans?spans[count]:prepared[count];
            item={static_cast<u16>(at),static_cast<u16>(end),static_cast<u16>(script>>3),static_cast<u16>(directions?directions[at]:0),static_cast<u64>(selected?4:0),0,flags};item.set_binding(fallback?font:nullptr);
            if(spans){continuity[count]=static_cast<u8>(script>>3);if(directions)directions[count]=directions[at];}}
        ++count;at=end;
    }
    return count;
}
// noinline keeps cache-miss scratch out of the retained-layout entry point under LTO.
// A projected flag owns one em-sized media cell, so its already-shaped source
// range is collapsed to one cluster in the caller's paragraph arrays; no
// character, font or glyph is replaced. Wrapping, bidi, selections and range
// geometry then see the same Unicode atom and advance as the atlas artwork.
__declspec(noinline) bool normalize_flag_cells(Utf8View text, ConstSlice<StyledSpan> spans,
    NativeAsciiParagraphStorage& storage, NativeParagraphShape& shape) noexcept {
    bool admitted = false;
    for (u32 i = 0; i < spans.count; ++i) admitted |= (spans[i].flags & text_style_custom_emoji) != 0;
    if (!admitted) return true;
    u32 cursor = 0, byte = 0, unit = 0, first_unit = 0, end_unit = 0;
    RegionalFlagRange flag{};
    const auto next_flag = [&]() noexcept {
        if (!regional_flag_next(text, &cursor, &flag)) return false;
        while (byte < flag.first_byte) {
            const auto cp = utf8_decode_one(text.data + byte, text.bytes - byte);
            if (!cp.valid || cp.width > flag.first_byte - byte) return false;
            byte += cp.width; unit += cp.codepoint > 0xffff ? 2u : 1u;
        }
        first_unit = unit;
        // All accepted RI/tag identities consist solely of four-byte scalars.
        unit += (flag.end_byte - flag.first_byte) / 2; byte = flag.end_byte;
        end_unit = unit; return true;
    };
    bool have_flag = next_flag(), changed = false;
    u32 written = 0;
    for (u32 read = 0; read < shape.cluster_count;) {
        const auto original = storage.clusters[read];
        while (have_flag && end_unit <= original.source_first) have_flag = next_flag();
        const u32 style = original.flags >> native_paragraph_style_shift;
        if (style >= spans.count) return false;
        if (!have_flag || original.source_first != first_unit ||
            !(spans[style].flags & text_style_custom_emoji)) {
            storage.clusters[written++] = original; ++read; continue;
        }
        const float em = spans[style].em;
        if (!(em > 0) || em > 65536) return false;
        auto cell = original;
        u32 last = read, glyph_end = cell.glyph_first;
        while (last < shape.cluster_count && storage.clusters[last].source_first < end_unit) {
            const auto& part = storage.clusters[last];
            const u32 part_style = part.flags >> native_paragraph_style_shift;
            if (part.source_end > end_unit || part.glyph_first != glyph_end ||
                part_style >= spans.count || !(spans[part_style].flags & text_style_custom_emoji) ||
                spans[part_style].em != em || part.glyph_count > shape.glyph_count - glyph_end) return false;
            glyph_end += part.glyph_count; ++last;
        }
        if (last == read || storage.clusters[last - 1].source_end != end_unit ||
            glyph_end <= cell.glyph_first || glyph_end > shape.glyph_count) return false;
        cell.source_end = end_unit; cell.glyph_count = glyph_end - cell.glyph_first; cell.width = em;
        cell.flags &= ~(native_paragraph_nonvisual | native_paragraph_metric_group);
        for (u32 g = cell.glyph_first; g < glyph_end; ++g) storage.placements[g] = {};
        storage.placements[cell.glyph_first].advance = em;
        for (u32 u = first_unit; u < end_unit; ++u) storage.source_glyphs[u] = cell.glyph_first;
        storage.clusters[written++] = cell; read = last; changed = true;
    }
    shape.cluster_count = written;
    // Nominal glyph reductions must consume the normalized placements/widths.
    if (changed) for (u32 c = 0; c < written; ++c)
        if (!(storage.clusters[c].flags & native_paragraph_nonvisual))
            storage.clusters[c].flags &= ~native_paragraph_metric_group;
    return true;
}

__declspec(noinline) bool prepare_styled_shape(NativeTextContext& owner,const TextSystem& system,const NativeFormat* format,Utf8View text,
    ConstSlice<TextStyleSpan> styles,float em,bool geometry,bool editor,u8 presentation,bool alphabetic,bool fallback=false,bool rtl=false,bool mixed=false,bool plain_tried=false) noexcept {
    constexpr u32 font_flags=text_style_bold|text_style_italic|text_style_monospace;
    const bool emoji=presentation!=0;
    // Font preparation resets the transient arena; complete it before storing
    // any resolved span there.
    owner.last_valid=false;owner.prefer_multiline=false;if(text.bytes>native_text_maximum_bytes)return false;
    const u32 scratch_bytes=maximum(4096u,(text.bytes+31u)&~31u);
    auto* script_bytes=static_cast<u8*>(_alloca(scratch_bytes+(mixed?text.bytes:0)));auto* directions=script_bytes+scratch_bytes;
    if(mixed){if(!directional_scripts(owner,text,script_bytes,directions))return false;}else if(rtl){for(u32 i=0;i<text.bytes;++i)script_bytes[i]=8;}else if(alphabetic&&!alphabetic_scripts(owner,text,script_bytes))return false;
    // Preparation may reset the arena. Stage fallback spans in unused script
    // scratch; source-unit fields temporarily carry script and direction.
    const u32 replay_first=(text.bytes+alignof(StyledSpan)-1)&~(alignof(StyledSpan)-1);
    Slice<StyledSpan> replay{reinterpret_cast<StyledSpan*>(script_bytes+replay_first),fallback?static_cast<u32>((scratch_bytes-replay_first)/sizeof(StyledSpan)):0};
    // A fallback pass that maps nothing to another face only repeats a failed
    // plain pass. Without that pass (text of base-less ignorables) it is the shape.
    bool changed=false;u32 count=presentation_spans(text,styles,presentation,nullptr,nullptr,&system,format,alphabetic?script_bytes:nullptr,fallback,&changed,editor,mixed?directions:nullptr,replay);if(!count||count>text.bytes||(fallback&&!changed&&!rtl&&!mixed&&plain_tried))return false;
    arena_reset(&owner.scratch);auto spans=take<StyledSpan>(owner.scratch,count);if(!spans.data)return false;
    auto* resolved=spans.data;
    if(count<=replay.count){for(u32 i=0;i<count;++i){resolved[i]=replay[i];script_bytes[i]=static_cast<u8>(replay[i].first_unit);if(mixed)directions[i]=static_cast<u8>(replay[i].end_unit);}}
    else if(presentation_spans(text,styles,presentation,resolved,script_bytes,fallback?&system:nullptr,fallback?format:nullptr,alphabetic?script_bytes:nullptr,fallback,nullptr,editor,mixed?directions:nullptr)!=count)return false;
    NativeSpanAnalysis<StyledSpan> continuity{script_bytes,resolved};
    auto analysis=take<u16>(owner.scratch,mixed?count:0);if(mixed&&!analysis.data)return false;
    const auto prefix=arena_mark(&owner.scratch);
    u32 at=0,units=0;UnicodeGraphemeState graphemes{};
    const bool classify=!system.independent_letters&&(count>1||alphabetic);
    for(u32 i=0;i<count;++i){auto& span=resolved[i];span.first_unit=static_cast<u16>(units);
        u32 requested=span.flags&font_flags;if(text_system_fixed_weight(system,span.flags))requested&=~text_style_bold;
        const bool selected=(continuity[i]&4)!=0;
        if(!span.binding())span.set_binding(binding(owner,format,selected?emoji_font_flag:requested));if(!span.binding()||(selected&&!emoji_profile(owner,*span.binding())))return false;
        if(emoji&&!mixed&&!selected&&!latin_text({text.data+span.first_byte,static_cast<u32>(span.end_byte)-span.first_byte},nullptr,false,alphabetic))return false;
        const float size=text_system_font_size(system,span.flags);span.em=size!=system.em_size?size:em;
        const auto plan=classify&&!selected?native_run_plan_nominal(owner.store,span.binding()->plan):NativeNominalPlan{};
        const auto* instance=classify&&!selected?native_font_instance_get(*owner.store.instances,owner.store.slots[span.binding()->plan.slot-1].instance):nullptr;
        const auto* font=instance?native_font_get(*owner.store.instances->fonts,instance->face):nullptr;bool plain=font!=nullptr&&(continuity[i]&~7u)<128;
        while(at<span.end_byte){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid||cp.width>span.end_byte-at)return false;
            if(editor||classify||emoji){bool boundary=false;if(!unicode_grapheme_next(&graphemes,cp.codepoint,&boundary))return false;
                if(at==span.first_byte){continuity[i]|=boundary?2:0;
                    // The explicit font override separates a standalone skin
                    // modifier from a preceding ordinary-font grapheme.
                    const bool modifier_cut=selected&&i&&!(continuity[i-1]&4)&&cp.codepoint-0x1f3fbu<5;
                    const bool ordinary_cut=i&&!selected&&!(continuity[i-1]&4);
                    const bool font_cut=ordinary_cut&&(resolved[i-1].binding()->font.slot!=span.binding()->font.slot||resolved[i-1].binding()->font.generation!=span.binding()->font.generation);
                    const bool paint_cut=ordinary_cut&&(editor||fallback)&&resolved[i-1].em==span.em&&
                        !((resolved[i-1].flags^span.flags)&(text_style_underline|text_style_strikethrough));
                    if(editor&&!rtl&&!mixed&&!boundary&&!modifier_cut&&!font_cut&&!paint_cut&&(!selected||(i&&((selected&&resolved[i-1].em!=span.em)||((continuity[i-1]^continuity[i])&4)))))return false;}}
            // CRLF and join-control cuts still need the control-itemization
            // policy; font-dependent paint cuts can change their clusters. With
            // browser invisibles on, a leading ZWNJ is the hidden-ignorable
            // substitute and opens a script run (also after a line break), so
            // it is not rejected here. Nothing joins across a hard line break.
            if(!rtl&&!mixed&&!selected&&at==span.first_byte&&at&&((cp.codepoint==0x200c&&!text_browser_invisibles())||
               (cp.codepoint==0x200d&&text.data[at-1]!='\n'&&text.data[at-1]!='\r')||(cp.codepoint==10&&text.data[at-1]=='\r')))return false;
            if(plain){u16 glyph=0;plain=nominal_scalar_is_simple(cp.codepoint);
                if(plain){const u32 offset=cp.codepoint-32u;if(offset<plan.ascii_count)glyph=static_cast<u16>(plan.ascii_first+offset);
                    else plain=sfnt_cmap_glyph(font->cmap,cp.codepoint,0,&glyph)==SfntStatus::ok;
                    plain=plain&&glyph&&(!plan.participation.count||!(plan.participation[glyph/64]&(1ull<<(glyph%64))));}}
            at+=cp.width;units+=cp.codepoint>65535?2u:1u;}
        span.end_unit=static_cast<u16>(units);if(plain)continuity[i]|=1;
    }
    const auto same_plan=[](const StyledSpan& a,const StyledSpan& b) noexcept {return a.binding()->plan.slot==b.binding()->plan.slot&&a.binding()->plan.generation==b.binding()->plan.generation&&((a.binding()->key^b.binding()->key)&0x0c000000u)==0&&a.em==b.em;};
    const auto paint_identity=[&](u32 byte) noexcept {u32 lo=0,hi=styles.count;while(lo<hi){const u32 mid=lo+(hi-lo)/2;
        if(styles[mid].byte_offset+styles[mid].byte_count<=byte)lo=mid+1;else hi=mid;}return lo<styles.count&&styles[lo].byte_offset<=byte?lo+1:0;};
    // A new font-property range can restart neutral script context after a
    // fallback symbol; keep its original start, since a size-only cut through a
    // selector differs from a complete symbol or a font/decoration transition.
    // Prior-family availability comes from prepared bindings: acquiring one here
    // could reset the arena holding these spans.
    // With tracking the full-shaping script cuts stay, including a neutral space
    // after an emoji: nominal-space coalescing would change float accumulation
    // in caret endpoints and decoration ranges despite identical glyph origins.
    if(mixed&&!system.independent_letters&&!system.tracking){u32 written=0,property_start=0,property_end=0,property_style=0,property_flags=0,previous_property_flags=0;bool neutral_property=false;
        for(u32 i=0;i<count;){auto item=resolved[i];u16 policy=continuity[i];u8 level=directions[i];u32 last=i+1;
            if(editor&&styles.count)while(property_end<=item.first_byte){const u32 previous_start=property_start;property_start=property_end;
                neutral_property=previous_start<property_start;for(u32 byte=previous_start;neutral_property&&byte<property_start;++byte)neutral_property=text.data[byte]==' ';
                while(property_style<styles.count&&styles[property_style].byte_offset+styles[property_style].byte_count<=property_start)++property_style;
                previous_property_flags=property_flags;property_flags=property_style<styles.count&&styles[property_style].byte_offset<=property_start?styles[property_style].flags:0;
                property_end=font_span_end(styles,property_start,text.bytes,property_style,system,format,property_flags);if(property_end<=property_start)return false;
            }
            bool resumed_context=editor&&i&&((continuity[i-1]&4)||((continuity[i-1]&~7u)==128))&&property_start;
            if(resumed_context&&property_start>=resolved[i-1].first_byte&&(!(continuity[i-1]&2)||property_start>resolved[i-1].first_byte))
                resumed_context=!(property_flags&(text_style_underline|text_style_strikethrough))&&span_size(system,format,property_flags)==em&&
                    same_query(font_query(owner,format,requested_font(system,property_flags)),font_query(owner,format,0));
            if(resumed_context&&!neutral_property){const bool font_changed=!same_query(font_query(owner,format,requested_font(system,previous_property_flags)),font_query(owner,format,requested_font(system,item.flags)));
                resumed_context=font_changed&&!(continuity[i-1]&2)&&span_size(system,format,previous_property_flags)!=item.em;const u32 requested=requested_font(system,previous_property_flags);
                for(u32 b=0;b<owner.binding_count;++b){const auto& prior=owner.bindings[b];if((prior.key&binding_default)&&prior.font_flags==requested&&(prior.format==format||same_font(prior.format->font,format->font))){resumed_context|=(prior.key&binding_missing_family)==0&&(font_changed||(item.em==em&&span_size(system,format,previous_property_flags)!=em)||((previous_property_flags^item.flags)&(text_style_underline|text_style_strikethrough))||(continuity[i-1]&2)||property_start<resolved[i-1].first_byte);break;}}
            }
            // A size or decoration override resumes nominal Latin after emoji,
            // even when the neutral space inherited the preceding script.
            const bool fresh_context=resumed_context||(written&&(continuity[written-1]&4)&&(item.em!=em||(item.flags&(text_style_underline|text_style_strikethrough)))&&last<count&&(continuity[last]&1))||(policy&~7u)==128||(written&&(continuity[written-1]&~7u)==128&&
                paint_identity(resolved[written-1].first_byte)!=paint_identity(item.first_byte));
            if(last<count&&(!editor||resumed_context||(item.flags&(text_style_underline|text_style_strikethrough))||(item.em!=em&&(continuity[last]&1))||!same_query(font_query(owner,format,requested_font(system,item.flags)),font_query(owner,format,requested_font(system,0))))&&
               (item.binding()->key&(binding_default|binding_missing_family))==binding_default&&!(level&1)&&level==directions[last]&&fresh_context&&!(policy&4)&&!(continuity[last]&4)&&(policy&~7u)!=64&&(continuity[last]&~7u)!=64&&
               (!written||resumed_context||!same_plan(resolved[written-1],item))&&same_plan(item,resolved[last])&&item.flags==resolved[last].flags&&
               paint_identity(item.first_byte)==paint_identity(resolved[last].first_byte)&&((policy^continuity[last])&~7u)){
                bool spaces=true;for(u32 byte=item.first_byte;spaces&&byte<item.end_byte;++byte)spaces=text.data[byte]==' ';
                bool compatible=(continuity[last]&1)!=0;
                if(spaces&&!compatible){const auto* plan=native_run_plan_get(owner.store,item.binding()->plan);
                    compatible=plan&&plan->substitutions&&!plan->substitutions->lookups.count;
                    for(u32 byte=resolved[last].first_byte;compatible&&byte<resolved[last].end_byte;){const auto cp=utf8_decode_one(text.data+byte,resolved[last].end_byte-byte);
                        compatible=cp.valid&&nominal_scalar_is_simple(cp.codepoint);byte+=cp.width;}
                }
                // A nominal space can carry a resumed size context even when
                // the following alphabetic glyph needs shaping. A participating
                // space retains the old script boundary and accumulation order.
                if(spaces&&(compatible||(resumed_context&&neutral_property&&(policy&1)&&span_size(system,format,previous_property_flags)!=item.em))){item.end_byte=resolved[last].end_byte;item.end_unit=resolved[last].end_unit;
                    policy=static_cast<u16>((continuity[last]&~7u)|(policy&2)|((policy&continuity[last])&1));++last;}
            }
            resolved[written]=item;continuity[written]=policy;directions[written]=level;++written;i=last;
        }count=written;
    }
    // A font override that resolves to the adjacent physical face does not
    // establish a Common script item. Its shared-script frontend is still
    // required; do not substitute separately shaped runs at that boundary.
    for(u32 i=1;i<count;++i)if(((continuity[i-1]^continuity[i])&4)&&resolved[i-1].binding()->plan.slot==resolved[i].binding()->plan.slot&&
        resolved[i-1].binding()->plan.generation==resolved[i].binding()->plan.generation)return false;
    // The retained editor merges runs by font and size properties and keeps
    // underline/strike boundaries; color-only effects do not split runs. Han
    // fallback items stay separate (their paint cuts were removed during mapping).
    if(editor){const auto weight=format->font.properties.weight;const auto font_style=format->font.properties.style;bool base_monospace=format->font.family.count==8;
        constexpr u16 expected[]{'c','o','n','s','o','l','a','s'};for(u32 i=0;base_monospace&&i<8;++i){const auto c=format->font.family[i];base_monospace=static_cast<u16>(c>='A'&&c<='Z'?c+32:c)==expected[i];}
        const auto properties=[&](const StyledSpan& span) noexcept {const u32 flags=span.flags;if(span.binding()->font_flags==emoji_font_flag)return flags&(text_style_underline|text_style_strikethrough);return static_cast<u32>((!text_system_fixed_weight(system,flags)&&(flags&text_style_bold))?600u:weight)|
            (static_cast<u32>((flags&text_style_italic)?2u:font_style)<<16)|((!base_monospace&&(flags&text_style_monospace))?1u<<24:0)|
            ((flags&text_style_underline)?1u<<25:0)|((flags&text_style_strikethrough)?1u<<26:0);};
        u32 merged=0;for(u32 i=0;i<count;++i){if(merged&&(continuity[i]&~7u)!=han_script&&static_cast<u32>(resolved[i].end_unit)-resolved[merged-1].first_unit<=16000&&same_plan(resolved[merged-1],resolved[i])&&(((continuity[merged-1]^continuity[i])&~7u)==0||((continuity[merged-1]|continuity[i])&~7u)==8)&&(!mixed||directions[merged-1]==directions[i])&&properties(resolved[merged-1])==properties(resolved[i])){resolved[merged-1].end_byte=resolved[i].end_byte;resolved[merged-1].end_unit=resolved[i].end_unit;
                continuity[merged-1]=static_cast<u16>((continuity[merged-1]&0xfffeu)|((continuity[merged-1]&continuity[i])&1));}
            else{resolved[merged]=resolved[i];continuity[merged]=continuity[i];if(mixed)directions[merged]=directions[i];++merged;}}count=merged;}
    if(classify){
        if(alphabetic){
            // Nominal eligibility belongs to the original font/effect range.
            // A newly inserted script cut cannot make half that range nominal.
            for(u32 first=0;first<count;){u32 last=first+1;bool plain=(continuity[first]&1)!=0;const u32 paint=paint_identity(resolved[first].first_byte);
                while(last<count&&same_plan(resolved[last],resolved[first])&&paint_identity(resolved[last].first_byte)==paint){plain=plain&&(continuity[last]&1)!=0;++last;}
                if(!plain)for(u32 i=first;i<last;++i)continuity[i]&=static_cast<u16>(~1u);first=last;}
        }
        // Initial nominal property ranges retain paragraph direction.
        // Fallback or script cuts cannot turn part of a complex range simple.
        if(mixed){u32 property_style=0;for(u32 first=0;first<count;){u32 last=first+1;bool plain=(continuity[first]&1)!=0&&!(resolved[first].binding()->key&binding_missing_family);if(!plain)break;const u32 paint=paint_identity(resolved[first].first_byte);
            while(property_style<styles.count&&styles[property_style].byte_offset+styles[property_style].byte_count<=resolved[first].first_byte)++property_style;
            const u32 property_end=editor?font_span_end(styles,resolved[first].first_byte,text.bytes,property_style,system,format,resolved[first].flags):text.bytes;if(!property_end)return false;
            while(plain&&last<count&&(editor?resolved[last].first_byte<property_end:paint_identity(resolved[last].first_byte)==paint)&&((continuity[last]^continuity[first])&4)==0){plain=(continuity[last]&1)!=0&&!(resolved[last].binding()->key&binding_missing_family);++last;}
            if(!plain)break;for(u32 i=first;i<last;++i)directions[i]=0;first=last;}}
        // A paint boundary inside a grapheme cannot divide its shaping.
        // Close eligibility over these dependencies before joining peers.
        for(u32 first=0;first<count;){u32 last=first+1;bool plain=(continuity[first]&1)!=0;
            while(last<count&&same_plan(resolved[last],resolved[first])&&!(continuity[last]&2)){plain=plain&&(continuity[last]&1)!=0;++last;}
            if(!plain)for(u32 i=first;i<last;++i)continuity[i]&=static_cast<u16>(~1u);first=last;}
    }
    if((rtl||mixed)&&classify){
        // Paint cuts cannot make one side of a shared font/script context
        // nominal: the other side may kern or substitute across that cut.
        for(u32 first=0;first<count;){u32 last=first+1;bool plain=(continuity[first]&1)!=0;
            while(last<count&&same_plan(resolved[last],resolved[first])&&((continuity[last]^continuity[first])&~7u)==0&&
                  (!mixed||((directions[last]^directions[first])&1)==0)){plain=plain&&(continuity[last]&1)!=0;++last;}
            if(!plain)for(u32 i=first;i<last;++i)continuity[i]&=static_cast<u16>(~1u);first=last;
        }
    }
    // Even levels can differ while sharing an LTR shaping context (for
    // example numbers followed by a space outside the RTL embedding).
    // Keep full levels on spans for layout; shaping needs their parity.
    u32 cached_space_span=0xffffffffu;bool cached_space=false;
    const auto same_group=[&](u32 a,u32 b) noexcept {const u32 byte=resolved[a].first_byte;
        const bool crlf_cut=byte&&text.data[byte]=='\n'&&text.data[byte-1]=='\r';
        const u32 a_script=continuity[a]&~7u,b_script=continuity[b]&~7u;
        bool compatible=a_script==b_script;
        if(!compatible&&editor&&mixed&&(resolved[a].binding()->key&binding_missing_family)&&
           ((a_script==arabic_script&&b_script==128)||(a_script==128&&b_script==arabic_script))){
            const u32 index=a_script==128?a:b;
            if(cached_space_span!=index){cached_space_span=index;cached_space=true;const auto& common=resolved[index];
                for(u32 at=common.first_byte;cached_space&&at<common.end_byte;){const auto cp=utf8_decode_one(text.data+at,common.end_byte-at);
                    cached_space=cp.valid&&(cp.codepoint==32||native_text_bidi_format(cp.codepoint));if(!cp.valid)break;at+=cp.width;}}
            compatible=cached_space;
        }
        return !crlf_cut&&static_cast<u32>(resolved[a].end_unit)-resolved[b].first_unit<=16000&&same_plan(resolved[a],resolved[b])&&compatible&&((continuity[a]^continuity[b])&5u)==0&&(!mixed||((directions[a]^directions[b])&1)==0);};
    // Brackets share the enclosed Arabic drawing group: opening brackets join
    // forward and closing brackets backward. Preserve the outer word groups
    // even when fallback resolves them to the same physical font. The editor
    // also retains neutral punctuation as Common until this reduction.
    if(mixed){u32 merged=0;u8 previous_mirrored=0;for(u32 i=0;i<count;++i){
        const u32 script=continuity[i]&~7u;const bool common=editor&&script==128&&(resolved[i].binding()->key&binding_missing_family);
        bool neutral=(script==arabic_script||common)&&(directions[i]&1);u8 mirrored=0;for(u32 byte=resolved[i].first_byte;neutral&&byte<resolved[i].end_byte;){const auto cp=utf8_decode_one(text.data+byte,resolved[i].end_byte-byte);if(!cp.valid)return false;
            neutral=(native_script_class(cp.codepoint).flags&native_script_contextual)!=0;
            if(unicode_properties(cp.codepoint)->has(UnicodeFlag::bidi_mirrored)){const auto type=unicode_bidi_bracket(cp.codepoint).type;mirrored|=type==UnicodeBracketType::open?1u:type==UnicodeBracketType::close?2u:3u;}byte+=cp.width;}if(!neutral)mirrored=0;
        const bool common_pair=merged&&editor&&((script==arabic_script&&(continuity[merged-1]&~7u)==128&&(resolved[merged-1].binding()->key&binding_missing_family))||(common&&(continuity[merged-1]&~7u)==arabic_script));
        if(merged&&((previous_mirrored&1)||(mirrored&2))&&(script==arabic_script||common)&&(directions[i]&1)&&
           (script==(continuity[merged-1]&~7u)||common_pair)&&same_plan(resolved[i],resolved[merged-1])&&
           static_cast<u32>(resolved[i].end_unit)-resolved[merged-1].first_unit<=16000&&((continuity[i]^continuity[merged-1])&5u)==0&&directions[i]==directions[merged-1]&&resolved[i].flags==resolved[merged-1].flags&&
           paint_identity(resolved[i].first_byte)==paint_identity(resolved[merged-1].first_byte)){
            if(common_pair){if(script==arabic_script)resolved[merged-1].set_binding(resolved[i].binding());continuity[merged-1]=static_cast<u16>(arabic_script|(continuity[merged-1]&7u));}
            resolved[merged-1].end_byte=resolved[i].end_byte;resolved[merged-1].end_unit=resolved[i].end_unit;
        }else{resolved[merged]=resolved[i];continuity[merged]=continuity[i];directions[merged]=directions[i];++merged;}previous_mirrored=mirrored;}count=merged;}
    if(mixed)for(u32 i=0;i<count;++i)analysis[i]=static_cast<u16>((static_cast<u16>(script_bytes[i])<<8)|directions[i]);
    // In a standalone Common paragraph, the requested face's proved-inert
    // glyphs retain nominal metric groups. Fallback and explicit typography
    // still require shaped accumulation, even when glyph execution is inert.
    bool common_only=mixed;for(u32 i=0;i<count;++i)common_only=common_only&&(continuity[i]&~7u)==128;
    u32 longest=0,longest_units=0,longest_ordinary=0;for(u32 i=0;i<count;){u32 last=i+1;while(last<count&&same_group(last,i))++last;
        const u32 bytes=static_cast<u32>(resolved[last-1].end_byte)-resolved[i].first_byte;longest=maximum(longest,bytes);
        longest_units=maximum(longest_units,static_cast<u32>(resolved[last-1].end_unit)-resolved[i].first_unit);
        if((continuity[i]&~7u)!=devanagari_script)longest_ordinary=maximum(longest_ordinary,bytes);i=last;}
    NativeAsciiParagraphStorage paragraph_buffers{};NativeParagraphShape shape{};Slice<char> saved{};Slice<TextStyleSpan> saved_styles{};
    Slice<SfntFontLineParts> parts{};Slice<NativeParagraphRun> runs{};Slice<NativeParagraphLine> lines{};SfntStatus status=SfntStatus::ok;
    for(u32 attempt=0;attempt<2;++attempt){if(!arena_rewind(&owner.scratch,prefix))return false;const u32 factor=attempt?4u:1u,glyphs=text.bytes*factor+16;
        saved=take<char>(owner.scratch,text.bytes);saved_styles=take<TextStyleSpan>(owner.scratch,styles.count);parts=take<SfntFontLineParts>(owner.scratch,count);
        // Every emitted run owns at least one source unit. Reuse the already
        // decoded UTF-16 length for both retained runs and borrowed boundaries.
        runs=take<NativeParagraphRun>(owner.scratch,units);lines=take<NativeParagraphLine>(owner.scratch,geometry?units+1:0);if(!saved.data||(styles.count&&!saved_styles.data)||!spans.data||!parts.data||!runs.data||(geometry&&!lines.data))return false;
        // Devanagari consumes component/lookup/positioning scratch one cluster at
        // a time, so the first attempt sizes work scratch by the longest other
        // item; the retry restores the full allocation if a font expands more.
        const u32 work_source=attempt?longest:minimum(longest,maximum(64u,longest_ordinary));
        const bool compact=work_source<longest;
        paragraph_buffers={};paragraph_buffers.work=storage(owner.scratch,work_source,compact?maximum(128u,work_source+16):longest*factor+16,false,attempt!=0);
        if(compact){auto& work=paragraph_buffers.work;work.clusters=take<NativeLineCluster>(owner.scratch,longest_units);
            work.run.glyphs=take<u16>(owner.scratch,longest+16);work.run.placements=take<SfntPlacement>(owner.scratch,longest+16);
            work.run.character_glyphs=take<u32>(owner.scratch,longest_units);
            if(!work.clusters.data||!work.run.glyphs.data||!work.run.placements.data||!work.run.character_glyphs.data)return false;}
        paragraph_buffers.glyphs=take<u16>(owner.scratch,glyphs);paragraph_buffers.placements=take<SfntPlacement>(owner.scratch,glyphs);
        paragraph_buffers.source_glyphs=take<u32>(owner.scratch,units);paragraph_buffers.clusters=take<NativeParagraphCluster>(owner.scratch,units);shape={};
        if(!paragraph_buffers.glyphs.data||!paragraph_buffers.placements.data||!paragraph_buffers.source_glyphs.data||!paragraph_buffers.clusters.data)return false;
        for(u32 i=0;i<count;++i){const auto* m=native_run_plan_metrics(owner.store,resolved[i].binding()->plan);
            if(!m||sfnt_font_line_parts(*m,resolved[i].em,parts.data+i)!=SfntStatus::ok)return false;}
        u32 span=0;bool nominal=true;
        auto group_boundaries=boundary_storage(runs);group_boundaries.deferred=true;
        // Han/kana itemization uses sequential cluster metrics even when the
        // selected face has no active substitutions or positioning lookups.
        for(u32 i=0;i<count;++i){const auto script=continuity[i]&~7u;
            if(script==han_script||script==kana_script||script==hiragana_script||script==hangul_script||script==old_hangul_script)nominal=false;}
        for(u32 first=0;first<count;){u32 last=first+1;while(last<count&&same_group(last,first))++last;
            auto segment=paragraph_buffers;segment.glyphs={paragraph_buffers.glyphs.data+shape.glyph_count,paragraph_buffers.glyphs.count-shape.glyph_count};
            segment.placements={paragraph_buffers.placements.data+shape.glyph_count,paragraph_buffers.placements.count-shape.glyph_count};
            segment.source_glyphs={paragraph_buffers.source_glyphs.data+shape.source_units,paragraph_buffers.source_glyphs.count-shape.source_units};
            segment.clusters={paragraph_buffers.clusters.data+shape.cluster_count,paragraph_buffers.clusters.count-shape.cluster_count};NativeParagraphShape part{};
            bool part_nominal=false;const auto& r=resolved[first];const Utf8View input{text.data+r.first_byte,static_cast<u32>(resolved[last-1].end_byte)-r.first_byte};
            u32 shaping_script=continuity[first]&~7u,shaping_first=first;
            if(shaping_script==128)for(u32 i=first+1;i<last;++i)if((continuity[i]&~7u)==arabic_script){shaping_script=arabic_script;shaping_first=i;break;}
            const bool deva=shaping_script==devanagari_script,tamil=shaping_script==tamil_script,sinhala=shaping_script==sinhala_script,khmer=shaping_script==khmer_script,myanmar=shaping_script==myanmar_script,tibetan=shaping_script==tibetan_script,form=form_script(static_cast<u16>(shaping_script));
            const auto form_kind=form_script_profile(static_cast<u16>(shaping_script));
            const auto* form_plan=form?form_profile(owner,*r.binding(),form_kind):nullptr;
            if(form&&!form_plan){status=SfntStatus::unsupported_sequence;break;}
            const auto* sinhala_plan=sinhala?sinhala_profile(owner,*r.binding(),system.independent_letters):nullptr;
            if(sinhala&&!sinhala_plan){status=SfntStatus::unsupported_sequence;break;}
            const auto* khmer_plan=khmer?khmer_profile(owner,*r.binding(),system.independent_letters):nullptr;
            if(khmer&&!khmer_plan){status=SfntStatus::unsupported_sequence;break;}
            const auto* myanmar_plan=myanmar?myanmar_profile(owner,*r.binding(),system.independent_letters):nullptr;
            if(myanmar&&!myanmar_plan){status=SfntStatus::unsupported_sequence;break;}
            const auto* tibetan_plan=tibetan?tibetan_profile(owner,*r.binding(),system.independent_letters):nullptr;
            if(tibetan&&!tibetan_plan){status=SfntStatus::unsupported_sequence;break;}
            const auto* tamil_plan=tamil?indic_profile(owner,*r.binding(),SfntIndicProfile::tamil):nullptr;
            if(tamil&&!tamil_plan){status=SfntStatus::unsupported_sequence;break;}
            const auto* deva_plan=deva?devanagari_profile(owner,*r.binding()):nullptr;
            if(deva&&!deva_plan){status=SfntStatus::unsupported_sequence;break;}
            status=tibetan_plan?native_tibetan_paragraph_shape(owner.store,*tibetan_plan,input,r.em,segment,group_boundaries,&part,resolved[0].em*system.tracking,mixed&&(directions[first]&1)!=0):myanmar_plan?native_myanmar_paragraph_shape(owner.store,*myanmar_plan,input,r.em,segment,group_boundaries,&part,resolved[0].em*system.tracking,mixed&&(directions[first]&1)!=0):khmer_plan?native_khmer_paragraph_shape(owner.store,*khmer_plan,input,r.em,segment,group_boundaries,&part,resolved[0].em*system.tracking,mixed&&(directions[first]&1)!=0):sinhala_plan?native_sinhala_paragraph_shape(owner.store,*sinhala_plan,input,r.em,segment,group_boundaries,&part,resolved[0].em*system.tracking,mixed&&(directions[first]&1)!=0):form_plan?indic_form_paragraph(form_kind,owner.store,*form_plan,input,r.em,segment,group_boundaries,&part,resolved[0].em*system.tracking,mixed&&(directions[first]&1)!=0):tamil?native_tamil_paragraph_shape(owner.store,*tamil_plan,input,r.em,segment,group_boundaries,&part,resolved[0].em*system.tracking,mixed&&(directions[first]&1)!=0):deva?native_devanagari_paragraph_shape(owner.store,*deva_plan,input,r.em,segment,group_boundaries,&part,resolved[0].em*system.tracking,mixed&&(directions[first]&1)!=0):r.binding()->font_flags==emoji_font_flag?native_emoji_paragraph_shape(owner.store,*r.binding()->emoji,input,r.em,segment,group_boundaries,&part,false,mixed&&(directions[first]&1)!=0,(continuity[first]&~7u)==arabic_script):
                shaping_script==arabic_script?native_arabic_paragraph_shape(owner.arabic_store,resolved[shaping_first].binding()->arabic,input,system.independent_letters,(directions[first]&1)!=0,r.em,segment,group_boundaries,&part,resolved[0].em*system.tracking):
                native_latin_paragraph_shape(owner.store,r.binding()->plan,input,system.independent_letters,r.em,segment,group_boundaries,&part,&part_nominal,resolved[0].em*system.tracking,
                    (continuity[first]&~7u)?alphabetic_profile(owner,*r.binding(),continuity[first]&~7u,system.independent_letters):nullptr,alphabetic&&(!(continuity[first]&1)||system.tracking!=0)&&(!common_only||(r.binding()->key&(binding_default|binding_missing_family))!=binding_default||system.independent_letters||system.tracking!=0),mixed?(directions[first]&1)!=0:rtl);
            nominal=nominal&&part_nominal;
            if(status!=SfntStatus::ok)break;
            if(!first)shape.vertical=part.vertical;
            for(u32 c=0;c<part.cluster_count;++c){auto& cluster=segment.clusters[c];cluster.source_first+=shape.source_units;cluster.source_end+=shape.source_units;cluster.glyph_first+=shape.glyph_count;
                while(span+1<count&&resolved[span].end_unit<=cluster.source_first)++span;
                if(editor&&cluster.source_end>resolved[span].end_unit){u32 covered=span;
                    if(!rtl&&!mixed){status=SfntStatus::unsupported_sequence;break;}
                    while(cluster.source_end>resolved[covered].end_unit){++covered;
                        if(covered==count||!same_plan(resolved[span],resolved[covered])||(mixed&&directions[span]!=directions[covered])){status=SfntStatus::unsupported_sequence;break;}}
                    if(status!=SfntStatus::ok)break;
                }
                // A drawing effect belongs to the cluster's leading source.
                // Keep following range identities even when an effect inside
                // the cluster itself produces no separately colored glyph.
                cluster.flags|=span<<native_paragraph_style_shift;
                if(!c||(segment.clusters[c-1].flags>>native_paragraph_style_shift)!=span)cluster.flags|=native_paragraph_metric_group;}
            if(status!=SfntStatus::ok)break;
            for(u32 u=0;u<part.source_units;++u)if(segment.source_glyphs[u]!=0xffffffffu)segment.source_glyphs[u]+=shape.glyph_count;
            shape.source_units+=part.source_units;shape.glyph_count+=part.glyph_count;shape.cluster_count+=part.cluster_count;first=last;
        }
        // Font/script cuts do not establish paragraph boundary context. Analyze
        // once after joining source coordinates and attaching style metadata.
        if(status==SfntStatus::ok)status=native_latin_paragraph_boundaries(text,{paragraph_buffers.clusters.data,shape.cluster_count},boundary_storage(runs));
        // Character spacing uses sequential cluster widths even when all
        // font spans otherwise qualify for nominal run-group accumulation.
        if(!nominal||system.tracking)for(u32 c=0;c<shape.cluster_count;++c)if(!(paragraph_buffers.clusters[c].flags&native_paragraph_nonvisual))paragraph_buffers.clusters[c].flags&=~native_paragraph_metric_group;
        if(status!=SfntStatus::output_too_small||attempt)break;++owner.storage_retries;
    }
    if(status!=SfntStatus::ok){++owner.execute_failed;return false;}
    // Fallback script cuts retain their metric accumulation boundaries even
    // when adjoining drawing runs happen to use the same physical face.
    if(alphabetic&&!system.tracking&&!fallback){
        // Script cuts end after shaping. Restore the original font/paint ranges
        // before fitting and decorations; a script change is not a style change.
        auto* remap=reinterpret_cast<u32*>(runs.data);u32 merged=0,paint=0,previous_paint=0xffffffffu;
        for(u32 i=0;i<count;++i){while(paint<styles.count&&styles[paint].byte_offset+styles[paint].byte_count<=resolved[i].first_byte)++paint;
            const u32 identity=paint<styles.count&&styles[paint].byte_offset<=resolved[i].first_byte?paint+1:0;
            if(merged&&static_cast<u32>(resolved[i].end_unit)-resolved[merged-1].first_unit<=16000&&(continuity[merged-1]&continuity[i]&1)&&identity==previous_paint&&same_plan(resolved[merged-1],resolved[i])&&resolved[merged-1].flags==resolved[i].flags){resolved[merged-1].end_byte=resolved[i].end_byte;resolved[merged-1].end_unit=resolved[i].end_unit;}
            else{resolved[merged]=resolved[i];parts[merged]=parts[i];continuity[merged]=continuity[i];++merged;previous_paint=identity;}remap[i]=merged-1;}
        u32 previous=0xffffffffu;
        for(u32 i=0;i<shape.cluster_count;++i){auto& c=paragraph_buffers.clusters[i];const u32 selected=remap[c.flags>>native_paragraph_style_shift];
            c.flags=(c.flags&((1u<<native_paragraph_style_shift)-1))|(selected<<native_paragraph_style_shift);
            if(selected==previous)c.flags&=~native_paragraph_metric_group;previous=selected;}
        count=merged;parts.count=merged;
    }
    spans.count=count;
    if (!normalize_flag_cells(text, {spans.data, spans.count}, paragraph_buffers, shape)) return false;
    copy_bytes(saved.data,text.data,text.bytes);copy_bytes(saved_styles.data,styles.data,styles.count*sizeof(TextStyleSpan));
    owner.last_text={saved.data,text.bytes};owner.last_styles={saved_styles.data,styles.count};owner.styled_spans=spans;owner.styled_parts=parts;owner.styled_lines=lines;
    owner.paragraph_storage=paragraph_buffers;owner.paragraph_shape=shape;owner.paragraph_runs=runs;owner.last_format=format;owner.last_em=em;owner.last_style_em=system.em_size;owner.last_fixed_weight=system.fixed_weight;
    owner.styled_analysis=analysis;owner.last_independent=system.independent_letters;owner.last_tracking=system.tracking;owner.last_paragraph=true;owner.last_styled=true;owner.last_mode=static_cast<u8>((editor?1:0)|(emoji?2:0)|(alphabetic?4:0)|(fallback?8:0)|(rtl?16:0)|(mixed?32:0)|(system.message_hit_policy?64:0));owner.last_valid=true;
    if(rtl||mixed){owner.styled_reset=prepare_line_reset(owner);if(owner.styled_reset.count!=shape.cluster_count){owner.last_valid=false;owner.prefer_multiline=false;return false;}}
    return true;
}
}
u32 native_text_candidate_style_capacity(Utf8View text,ConstSlice<TextStyleSpan> styles) noexcept {
    if(!text.data||!text.bytes||text.bytes>native_text_maximum_bytes||styles.count>kTextStyleSpanMaximum||(styles.count&&!styles.data))return false;
    const u32 count=presentation_spans(text,styles,2);
    if(latin_text(text,nullptr,true,true)&&!latin_text(text)){
        u32 extra=0;UnicodeScript previous=UnicodeScript::zzzz;
        for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return false;const auto value=native_script_class(cp.codepoint);
            if(value.flags&native_script_nonvisual){extra+=2;previous=UnicodeScript::zzzz;}
            else if(!(value.flags&native_script_contextual)){if(previous!=UnicodeScript::zzzz&&previous!=value.script)++extra;previous=value.script;}at+=cp.width;}
        return minimum(text.bytes,count+extra);}
    return count>1||styles.count?count:0;
}
u32 native_text_candidate_hit_style_capacity(const TextSystem& system) noexcept {
    const auto* owner=system.native_text;
    return owner&&owner->last_valid&&owner->last_styled&&(owner->last_mode&1)?owner->styled_parts.count:0;
}
// Retained-layout publication is a hot path even when shape preparation is cached.
#if defined(MAICORD_COMPACT_TEXT_ADAPTER)
#pragma optimize("t",on)
#endif
bool native_text_styled_plan_or_output(const TextSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,float height,
    ShapedText* output,TextMeasureResult* measure,TextWrapping wrapping,TextOverflow overflow,ConstSlice<TextHitRange> hits,NativeParagraphHitStorage* editor,
    Slice<NativeParagraphLineFit> fit,NativeParagraphLayout* planning,NativeTrimPlanScratch* plan_storage=nullptr,bool require_styled=false) noexcept {
    if(editor){if(!editor->output||output)return false;*editor->output={};if(editor->required_visual_runs)*editor->required_visual_runs=0;}
    if(overflow==TextOverflow::ellipsis&&!(height>0))return false;
    if(!system.native_text||!tracking_valid(system.tracking)||!text.bytes||text.bytes>native_text_maximum_bytes||!text.data||styles.count>kTextStyleSpanMaximum||(styles.count&&!styles.data)||(!output&&!measure)||hits.count>kTextHitSpanMaximum||(hits.count&&(!hits.data||!output)))return false;
    constexpr u32 font_flags=text_style_bold|text_style_italic|text_style_monospace;
    constexpr u32 size_flags=text_style_heading_1|text_style_heading_2|text_style_heading_3|text_style_subtext|text_style_picker_emoji|text_style_jumbo_emoji;
    constexpr u32 paint_flags=text_style_link|kTextStyleSyntaxMask|text_style_timestamp|text_style_interactive_hover|text_style_inline_role_color|text_style_custom_emoji;
    const u32 supported=font_flags|size_flags|paint_flags|native_decoration_flags|text_style_list|kTextStyleListDepthMask;
    u32 style_flags=0;
    // Vectorizing this short, strided reduction would gather eight style
    // records plus a scalar remainder; the scalar loop is smaller.
#pragma loop(no_vector)
    // Spans arrive folded (text_fold_style_sizes): sizes are heading-bit codes.
    for(u32 i=0;i<styles.count;++i){if((styles[i].flags&~supported)||styles[i].size)return false;style_flags|=styles[i].flags;}
    const bool decorations=(style_flags&native_decoration_flags)!=0;
    const bool geometry=decorations||hits.count;
    const auto& cached=*system.native_text;const bool known=cached.last_valid&&cached.last_styled&&((cached.last_mode&64)!=0)==system.message_hit_policy&&(cached.last_tracking==0)==(system.tracking==0)&&cached.last_text.count==text.bytes&&bytes_equal(cached.last_text.data,text.data,text.bytes);
    bool latin=false;if(!known)for(u32 i=0;i<text.bytes;++i){const auto cp=static_cast<u8>(text.data[i]);if((cp<32&&cp!=9&&cp!=10&&cp!=13)||cp>126)latin=true;}
    // Tracked Hebrew needs paragraph-end whitespace direction before spacing;
    // use the shared itemizer rather than the uniform-RTL shortcut.
    const bool rtl=!system.tracking&&(known?(cached.last_mode&16)!=0:hebrew_text(text));
    bool directional_emoji=false;
    bool mixed=known?(cached.last_mode&32)!=0:!rtl&&itemized_text(text,&directional_emoji);bool directional=rtl||mixed;
    if(directional&&editor&&system.independent_letters)return false;
    bool extended=directional,common=false,emoji=directional_emoji;
    if(known){extended|=(cached.last_mode&4)!=0;emoji=(cached.last_mode&2)!=0;}
    else if(latin&&!directional){
        const bool plain_latin=latin_text(text);
        extended=!plain_latin&&latin_text(text,nullptr,true,true);
        common=!extended&&!plain_latin&&latin_text(text,nullptr,false);
        emoji|=!extended&&!common&&!plain_latin;
    }
    u8 presentation=0;
    if(emoji){TextEmojiCursor cursor{};TextEmojiRange range{};if(text_emoji_next(text,&cursor,&range)!=TextEmojiNext::range)return false;
        presentation=!range.first_byte&&range.end_byte==text.bytes?1:2;
        if(!known&&!directional&&presentation==2){u32 at=0;
            const auto ordinary=[&](u32 first,u32 end) noexcept {if(first==end)return true;const Utf8View part{text.data+first,end-first};
                if(!latin_text(part,nullptr,false,true))return false;if(!latin_text(part,nullptr,false))extended=true;return true;};
            for(;;){if(!ordinary(at,range.first_byte))return false;at=range.end_byte;const auto next=text_emoji_next(text,&cursor,&range);
                if(next==TextEmojiNext::invalid_input)return false;if(next==TextEmojiNext::end)break;}
            if(!ordinary(at,text.bytes))return false;
        }}
    if(system.message_hit_policy){if(emoji){mixed=true;directional=true;extended=true;}presentation=0;emoji=false;}
    const bool fallback_only=!require_styled&&text.bytes<=16000&&(directional||common||(!emoji&&!styles.count&&!hits.count&&!extended));
    const auto* format=resolve_format(system,wrapping);if(!format)return false;
    auto& owner=*system.native_text;const float em=format->em;++owner.attempts;
    // Resolve the omission face before retaining source scratch: a cache miss
    // can prepare a different fallback shape in that same arena.
    if(overflow==TextOverflow::ellipsis&&!editor&&
       !omission_sign(owner,format)&&!prepare_omission_sign(owner,system,format))return false;
    const u8 mode=static_cast<u8>((editor?1:0)|(emoji?2:0)|(extended?4:0)|(rtl?16:0)|(mixed?32:0)|(system.message_hit_policy?64:0));
    if(owner.last_valid&&owner.last_styled&&(owner.last_mode&119)==mode&&(!geometry||owner.styled_lines.count)&&owner.last_format==format&&owner.last_em==em&&owner.last_style_em==system.em_size&&owner.last_fixed_weight==system.fixed_weight&&owner.last_independent==system.independent_letters&&owner.last_tracking==system.tracking&&
       owner.last_text.count==text.bytes&&bytes_equal(owner.last_text.data,text.data,text.bytes)&&owner.last_styles.count==styles.count&&bytes_equal(owner.last_styles.data,styles.data,styles.count*sizeof(TextStyleSpan)))++owner.reuses;
    else if((fallback_only||!prepare_styled_shape(owner,system,format,text,styles,em,geometry,editor!=nullptr,presentation,extended))&&
        !prepare_styled_shape(owner,system,format,text,styles,em,geometry,editor!=nullptr,presentation,extended,true,rtl,mixed,!fallback_only))return false;
    const auto& shape=owner.paragraph_shape;const auto& s=owner.paragraph_storage;NativeParagraphLayout layout{};const float tab_stop=format->tab_stop;
    if(plan_storage){if(!plan_storage->allocate(owner,shape.cluster_count))return false;fit=plan_storage->fit;}
    const auto transient_mark=arena_mark(&owner.scratch);
    struct Rewind {Arena& arena;ArenaMark mark;~Rewind() noexcept {arena_rewind(&arena,mark);}} rewind{owner.scratch,transient_mark};
    const auto reset=directional?owner.styled_reset:ConstSlice<u8>{};if(directional&&reset.count!=shape.cluster_count)return false;
    // A measurement supplies reset eligibility but has no publication sink.
    RtlLineOutput rtl_output{owner,output};
    const NativeParagraphLineSink directional_output{&rtl_output,directional&&output?publish_rtl_line:nullptr,reset,directional?split_rtl_fallback_space:nullptr};
    const auto fitted=editor&&!directional&&!styles.count&&owner.styled_parts.count==1?
        native_prepared_paragraph_layout(shape,{s.clusters.data,shape.cluster_count},{s.placements.data,shape.glyph_count},width,tab_stop,wrapping==TextWrapping::wrap,owner.paragraph_runs,&layout,editor):
        native_prepared_styled_paragraph_layout(shape,{s.clusters.data,shape.cluster_count},{s.placements.data,shape.glyph_count},{owner.styled_parts.data,owner.styled_parts.count},width,tab_stop,wrapping==TextWrapping::wrap,owner.paragraph_runs,&layout,output?owner.styled_lines:Slice<NativeParagraphLine>{},editor,directional_output,overflow==TextOverflow::ellipsis?height:0,fit);
    if(fitted!=SfntStatus::ok){++owner.execute_failed;return false;}
    if(planning){*planning=layout;return true;}
    if(editor&&directional&&!native_directional_hits(owner,layout,*editor)){*editor->output={};return false;}
    if(overflow==TextOverflow::ellipsis&&((layout.flags&native_paragraph_emergency_break)||layout.width>width)){
        if(layout.line_count==1&&!editor){
            if(trim_single_line(owner,nullptr,system,format,text,width,height,wrapping,layout,output,measure,directional?&rtl_output:nullptr,hits))return true;}
        ++owner.does_not_fit;return false;}
    if(output){
        const auto* final_run=layout.run_count?owner.paragraph_runs.data+layout.run_count-1:nullptr;
        const u32 visible_glyphs=(layout.flags&native_paragraph_height_trimmed)?(final_run?final_run->glyph_first+final_run->glyph_count:0):shape.glyph_count;
        const u32 visible_source=(layout.flags&native_paragraph_height_trimmed)?(final_run?static_cast<u32>(final_run->source_first)+final_run->source_count:0):shape.source_units;
        if(visible_glyphs>output->glyph_capacity||shape.source_units>output->cluster_capacity||!output->glyphs||!output->clusters)return output_capacity_failure(owner);
        if(!directional)for(u32 i=0;i<visible_glyphs;++i){const auto& p=s.placements[i];output->glyphs[i]={p.advance,p.advance_offset,p.ascender_offset,s.glyphs[i],0};}
        const u32 published_glyphs=directional?rtl_output.written:visible_glyphs;const bool reshaped=directional;
        u32 decoration_count=0;if(decorations&&!native_styled_decorations(owner,layout,width,tab_stop,{output->decorations,output->decoration_capacity},&decoration_count,directional?output:nullptr))return false;
        u32 hit_count=0;
        if(hits.count){if(!output->hit_regions||!output->hit_region_capacity)return false;
            DecorationGraphemeCursor boundaries{};
            for(u32 h=0;h<hits.count;++h){const auto& hit=hits[h];
                if(!hit.unit_count||hit.first_unit>shape.source_units||hit.unit_count>shape.source_units-hit.first_unit)return false;
                if(!decoration_range(owner,layout,tab_stop,hit.first_unit,hit.first_unit+hit.unit_count,
                    [&](const NativeParagraphLine& line,float left,float width) noexcept {
                        if(!(width>0)||!(line.height>0))return true;
                        if(hit_count==output->hit_region_capacity)return false;
                        output->hit_regions[hit_count++]={left,line.top,decoration_add(left,width),decoration_add(line.top,line.height),hit.semantic_index,0};return true;
                    },directional?&boundaries:nullptr,directional?output:nullptr))return false;
            }
        }
        if(layout.run_count>output->run_capacity||published_glyphs>output->glyph_capacity||shape.source_units>output->cluster_capacity||published_glyphs>65535)return output_capacity_failure(owner);
        if(output->native_fonts()||output->run_count||!native_font_session_retain(owner.fonts))return false;
        auto* fonts=native_font_session_runs(owner.fonts);u32 retained=0,span=0;
        while(retained<layout.run_count){const auto& run=owner.paragraph_runs[retained];while(span+1<owner.styled_spans.count&&owner.styled_spans[span].end_unit<=run.source_first)++span;
            if(!native_font_run_retain(fonts,owner.styled_spans[span].binding()->font))break;++retained;}
        if(retained!=layout.run_count){span=0;for(u32 i=0;i<retained;++i){while(span+1<owner.styled_spans.count&&owner.styled_spans[span].end_unit<=owner.paragraph_runs[i].source_first)++span;native_font_run_release(fonts,owner.styled_spans[span].binding()->font);}
            auto* session=owner.fonts;native_font_session_release(&session);return false;}
        output->set_native_fonts(owner.fonts);
        if(!reshaped)for(u32 i=0;i<visible_source;++i)output->clusters[i]=static_cast<u16>(s.source_glyphs[i]);
        for(u32 i=visible_source;i<shape.source_units;++i)output->clusters[i]=0xffffu;span=0;
        for(u32 i=0;i<layout.run_count;++i){const auto& run=owner.paragraph_runs[i];while(span+1<owner.styled_spans.count&&owner.styled_spans[span].end_unit<=run.source_first)++span;const auto& style=owner.styled_spans[span];
            const float x=directional?output->runs[i].baseline_x:run.x;const u8 level=directional?output->runs[i].bidi_level:0;
            const u32 first_glyph=reshaped?output->runs[i].first_glyph:run.glyph_first,glyphs=reshaped?output->runs[i].glyph_count:run.glyph_count;
            output->runs[i]={style.binding()->font,x,run.baseline,style.em,first_glyph,glyphs,run.source_first,run.source_count,style.flags,level,false,true};}
        output->glyph_count=published_glyphs;output->run_count=layout.run_count;output->cluster_count=shape.source_units;output->decoration_count=decoration_count;output->hit_region_count=hit_count;output->width=layout.width;output->width_including_trailing_whitespace=layout.extent;output->height=layout.height;output->line_count=layout.line_count;++owner.shaped;++owner.paragraphs_shaped;
    }else{*measure={S_OK,layout.width,layout.extent,layout.height,shape.source_units,layout.line_count,TextShapeError::none};++owner.measured;++owner.paragraphs_measured;}
    return true;
}

#if defined(MAICORD_COMPACT_TEXT_ADAPTER)
#pragma optimize("",on)
#endif

bool native_text_candidate_styled_once(const TextSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,float height,
    ShapedText* output,TextMeasureResult* measure,TextWrapping wrapping,TextOverflow overflow,ConstSlice<TextHitRange> hits,NativeParagraphHitStorage* editor) noexcept {
    CatalogOperation catalog_operation(system.native_text);
    return native_text_styled_plan_or_output(system,text,styles,width,height,output,measure,wrapping,overflow,hits,editor,{},nullptr);
}
