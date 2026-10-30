#include "unicode_line_geometry.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/detail/paragraph_layout.hpp"
#include "maicord/text/unicode.hpp"
#include "layout_float.hpp"
namespace maicord {namespace {
using B=UnicodeLineBreak;
B kind(char c) noexcept {return unicode_properties(static_cast<u8>(c))->line_break();}
// LB rules for ASCII between a and b; visible is the class of the last
// non-space character before them (xx when none).
constexpr bool allowed(B a,B b,B visible) noexcept {
    if(a==B::cr&&b==B::lf)return false;if(a==B::cr||a==B::lf)return true;
    if(b==B::sp||b==B::cr||b==B::lf)return false;
    if(b==B::cl||b==B::cp||b==B::ex||b==B::is||b==B::sy)return false;
    if(visible==B::op)return false;
    if(visible==B::qu&&b==B::op)return false;
    if(a==B::sp)return true;
    if(a==B::qu||b==B::qu||b==B::hy||b==B::ba)return false;
    if((a==B::al&&b==B::nu)||(a==B::nu&&b==B::al))return false;
    if((a==B::pr||a==B::po)&&b==B::al)return false;
    if(((a==B::pr||a==B::po)&&(b==B::op||b==B::nu))||((a==B::op||a==B::hy||a==B::is||a==B::sy)&&b==B::nu))return false;
    if(a==B::nu&&(b==B::nu||b==B::pr||b==B::po))return false;
    if((a==B::cp||a==B::cl)&&(b==B::pr||b==B::po))return false;
    if((a==B::al&&b==B::al)||(a==B::is&&b==B::al)||((a==B::al||a==B::nu)&&b==B::op)||(a==B::cp&&(b==B::al||b==B::nu)))return false;
    return true;
}
// The classes of printable ASCII, TAB, CR and LF, and the rules above over all
// their pairs: one row of break bits per class a before the pair, indexed by b.
constexpr B classes[16]={B::al,B::nu,B::sp,B::op,B::cp,B::cl,B::ex,B::is,B::sy,B::qu,B::hy,B::ba,B::pr,B::po,B::cr,B::lf};
constexpr u32 space_id=2,no_visible=16;
struct Rules {u16 after[16],after_space[17];u8 id[static_cast<u32>(B::zwj)+1];};
constexpr Rules make_rules() noexcept {
    Rules r{};
    for(u32 a=0;a<16;++a)for(u32 b=0;b<16;++b)r.after[a]|=static_cast<u16>(allowed(classes[a],classes[b],classes[a])<<b);
    for(u32 v=0;v<17;++v)for(u32 b=0;b<16;++b)r.after_space[v]|=static_cast<u16>(allowed(B::sp,classes[b],v<16?classes[v]:B::xx)<<b);
    for(u32 c=0;c<16;++c)r.id[static_cast<u32>(classes[c])]=static_cast<u8>(c);
    return r;
}
constexpr Rules rules=make_rules();
float tab(float pen,float stop) noexcept {
    const float q=pen/stop;if(!(q>=0)||q>1000000)return -1;
    return static_cast<float>(static_cast<u32>(q)+1)*stop;
}
}
void native_ascii_paragraph_flags(Utf8View text,Slice<NativeParagraphCluster> clusters) noexcept {
    u32 next=0,previous=0,visible=no_visible;
    for(u32 at=0;at<text.bytes&&next<clusters.count;++at){const u32 b=rules.id[static_cast<u32>(kind(text.data[at]))];
        if(clusters[next].source_first==at){auto& c=clusters[next++];
            const u32 row=previous==space_id?rules.after_space[visible]:rules.after[previous];
            c.flags|=(b==space_id?native_paragraph_space:0)|(at&&(row>>b&1)?native_paragraph_break_before:0);}
        previous=b;if(b!=space_id)visible=b;}
}
SfntStatus native_line_trim_cut(ConstSlice<NativeParagraphCluster> clusters,float width,float extent,
    float box,float sign_width,bool force,ConstSlice<float> tabs,NativeLineTrimCut* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if((clusters.count&&!clusters.data)||(tabs.count&&!tabs.data)||!is_finite(width)||width<0||!is_finite(extent)||extent<0||
        !is_finite(box)||box<=0||!is_finite(sign_width)||sign_width<0)return SfntStatus::invalid_input;
    u32 tab_count=0;
    for(u32 i=0;i<clusters.count;++i){const auto& c=clusters[i];
        if(!is_finite(c.width)||c.width<0||(c.flags&(native_paragraph_newline|native_paragraph_crlf_prefix)))return SfntStatus::invalid_input;
        if(c.flags&native_paragraph_tab){if(tab_count==tabs.count||!is_finite(tabs[tab_count])||tabs[tab_count]<0)return SfntStatus::invalid_input;++tab_count;}}
    if(tab_count!=tabs.count)return SfntStatus::invalid_input;
    NativeLineTrimCut result{clusters.count,width,extent,false};
    if(!force&&width<=box){*output=result;return SfntStatus::ok;}
    float remaining=extent;const float available=layout_add(box,-sign_width);
    const auto remove=[&]() noexcept {
        const auto& c=clusters[result.clusters-1];const bool is_tab=(c.flags&native_paragraph_tab)!=0;
        const float next=layout_add(remaining,-(is_tab?tabs[tab_count-1]:c.width));
        // A cut that would leave a zero remainder is refused: the final cluster is indivisible.
        if(next==0)return false;
        remaining=next;--result.clusters;if(is_tab)--tab_count;return true;
    };
    // Height omission replaces a source suffix even when the line fits: start by removing one cluster.
    if(force&&result.clusters)remove();
    while(result.clusters&&remaining>available)if(!remove())break;
    while(result.clusters&&(clusters[result.clusters-1].flags&(native_paragraph_space|native_paragraph_tab)))if(!remove())break;
    // An intact line has no source range to replace; Windows marks it trimmed without drawing a sign.
    result.sign=result.clusters<clusters.count;
    if(result.sign)result.width=result.extent=layout_add(remaining,sign_width);
    // A height-trimmed indivisible whitespace line keeps its advance in the
    // reported width even when no source suffix can be replaced by a sign.
    else if(force)result.width=result.extent;
    if(!is_finite(result.width)||!is_finite(result.extent))return SfntStatus::resource_limit;
    *output=result;return SfntStatus::ok;
}
SfntStatus native_ascii_paragraph_shape(const NativeRunPlanStore& store,NativeRunPlanHandle plan,Utf8View input,bool independent,float em,
    const NativeAsciiParagraphStorage& s,NativeParagraphShape* output,float trailing_spacing) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(input.bytes>65535)return SfntStatus::resource_limit;
    if((input.bytes&&!input.data)||(s.source_glyphs.count&&!s.source_glyphs.data)||(s.clusters.count&&!s.clusters.data)||
       (s.glyphs.count&&!s.glyphs.data)||(s.placements.count&&!s.placements.data))return SfntStatus::invalid_input;
    for(u32 i=0;i<input.bytes;++i){const u8 c=static_cast<u8>(input.data[i]);if((c<32&&c!=9&&c!=10&&c!=13)||c>126)return SfntStatus::unsupported_sequence;}
    NativeParagraphShape shaped{};const auto* metrics=native_run_plan_metrics(store,plan);if(!metrics)return SfntStatus::invalid_input;
    auto status=sfnt_single_font_line(*metrics,em,&shaped.vertical);if(status!=SfntStatus::ok)return status;
    for(u32 at=0;at<input.bytes;){
        if(input.data[at]=='\r'||input.data[at]=='\n'||input.data[at]=='\t'){
            const u32 count=input.data[at]=='\r'&&at+1<input.bytes&&input.data[at+1]=='\n'?2u:1u;
            status=native_paragraph_append_control(s,shaped,count,input.data[at]=='\t'?native_paragraph_tab:native_paragraph_newline);if(status!=SfntStatus::ok)return status;
            at+=count;continue;
        }
        u32 end=at;while(end<input.bytes&&input.data[end]>=32)++end;
        NativeAsciiLineResult line{};status=native_ascii_line(store,plan,{input.data+at,end-at},independent,em,s.work,&line,{},trailing_spacing);if(status!=SfntStatus::ok)return status;
        status=native_paragraph_append_line(s,line,shaped);if(status!=SfntStatus::ok)return status;
        at=end;
    }
    native_ascii_paragraph_flags(input,{s.clusters.data,shaped.cluster_count});
    *output=shaped;return SfntStatus::ok;
}
namespace {
SfntStatus paragraph_layout(const NativeParagraphShape& shape,ConstSlice<NativeParagraphCluster> clusters,
    ConstSlice<SfntPlacement> placements,float width,float stop,bool wrap,Slice<NativeParagraphRun> runs,NativeParagraphLayout* output,NativeParagraphHitStorage* hits,ConstSlice<SfntFontLineParts> styles,Slice<NativeParagraphLine> lines,NativeParagraphLineSink sink,float trimming_height,Slice<NativeParagraphLineFit> fit) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(hits){if(!hits->output)return SfntStatus::invalid_input;reset_value(&(*hits->output));
        if((hits->clusters.count&&!hits->clusters.data)||(hits->lines.count&&!hits->lines.data)||(hits->advances.count&&!hits->advances.data))return SfntStatus::invalid_input;}
    if(!is_finite(trimming_height)||trimming_height<0||(trimming_height&&hits))return SfntStatus::invalid_input;
    if(clusters.count!=shape.cluster_count||placements.count!=shape.glyph_count||(clusters.count&&!clusters.data)||(placements.count&&!placements.data)||(runs.count&&!runs.data)||!is_finite(width)||width<=0||!is_finite(stop)||stop<=0||!is_finite(shape.vertical.height)||shape.vertical.height<=0||!is_finite(shape.vertical.baseline))return SfntStatus::invalid_input;
    if(shape.source_units>65535||(hits&&placements.count>native_hit_advance_mask))return SfntStatus::resource_limit;if((lines.count&&!lines.data)||(fit.count&&!fit.data))return SfntStatus::invalid_input;
    if(sink.trailing_reset.count&&(sink.trailing_reset.count!=clusters.count||!sink.trailing_reset.data))return SfntStatus::invalid_input;
    if(styles.count){if(!styles.data)return SfntStatus::invalid_input;
        if(hits&&(styles.count>65535||styles.count>hits->styles.count||!hits->styles.data))return SfntStatus::output_too_small;
        for(u32 i=0;i<styles.count;++i){const auto& s=styles[i];if(!is_finite(s.baseline)||!is_finite(s.descent)||s.baseline<0||s.descent<0||!(layout_add(s.baseline,s.descent)>0))return SfntStatus::invalid_input;}}
    bool nominal_groups=styles.count&&clusters.count&&(clusters[0].flags&native_paragraph_metric_group)&&!(clusters[0].flags&native_paragraph_nonvisual);float nominal_done=0,nominal_part=0;
    for(u32 i=0;nominal_groups&&i<clusters.count;++i){const auto& c=clusters[i];
        if(c.flags&(native_paragraph_tab|native_paragraph_newline))nominal_groups=false;
        if(c.flags&native_paragraph_metric_group){nominal_done=layout_add(nominal_done,nominal_part);nominal_part=0;}nominal_part=layout_add(nominal_part,c.width);
    }
    nominal_groups=nominal_groups&&!(clusters[clusters.count-1].flags&native_paragraph_space)&&(!wrap||layout_add(nominal_done,nominal_part)<width);
    NativeParagraphLayout layout{};u32 start=0,hit_count=0,advance_count=0;float top=0;bool hidden=false;
    do{
        const u32 hit_first=hit_count,run_first=layout.run_count;
        u32 end=start,last_break=start;float pen=0;
        while(end<clusters.count){const auto& c=clusters[end];if(c.flags&native_paragraph_newline){++end;break;}
            if(c.flags&native_paragraph_crlf_prefix){if(end+1>=clusters.count||!(clusters[end+1].flags&native_paragraph_newline))return SfntStatus::invalid_input;end+=2;break;}
            if(end>start&&(c.flags&native_paragraph_break_before))last_break=end;
            const float next=(c.flags&native_paragraph_tab)?tab(pen,stop):layout_add(pen,c.width);
            if(next<0||!is_finite(next))return SfntStatus::resource_limit;
            if(wrap&&!nominal_groups&&next>width&&!(c.flags&(native_paragraph_space|native_paragraph_tab))&&end>start){
                if(last_break>start){end=last_break;break;}if(!trimming_height){layout.flags|=native_paragraph_emergency_break;break;}}
            pen=next;++end;
        }
        // A zero-glyph final empty line contributes height but emits no run.
        float line_baseline=shape.vertical.baseline,line_height=shape.vertical.height;u32 ascent_style=0,descent_style=0;
        if(styles.count&&start<end){float ascent=0,descent=0;
            ascent_style=descent_style=clusters[start].flags>>native_paragraph_style_shift;
            for(u32 c=start;c<end;++c){const u32 style=clusters[c].flags>>native_paragraph_style_shift;const auto& s=styles[style];if(s.baseline>ascent)ascent_style=style;if(s.descent>descent)descent_style=style;ascent=maximum(ascent,s.baseline);descent=maximum(descent,s.descent);}
            line_baseline=ascent;line_height=layout_add(ascent,descent);}
        const u32 end_flags=end>start&&(clusters[end-1].flags&native_paragraph_newline)?native_line_fit_hard_end:0u;
        if(fit.data){if(layout.line_count==fit.count)return SfntStatus::output_too_small;
            fit[layout.line_count]={static_cast<u16>(start),static_cast<u16>(end-start),top,line_height,line_baseline,0,0,static_cast<u16>(end-start),static_cast<u16>(end_flags)};}
        if(trimming_height&&layout.line_count&&(hidden||layout_add(top,line_height)>trimming_height)){
            if(!hidden&&start&&!(clusters[start-1].flags&native_paragraph_newline)){
                if(!fit.data)return SfntStatus::unsupported_sequence;
                fit[layout.line_count-1].flags|=native_line_fit_force_sign;
            }
            if(fit.data){auto& line=fit[layout.line_count];line.height=line.baseline=0;line.flags|=native_line_fit_hidden;}
            hidden=true;layout.flags|=native_paragraph_height_trimmed;
            if(lines.data){if(layout.line_count==lines.count)return SfntStatus::output_too_small;lines[layout.line_count]={static_cast<u16>(layout.run_count),0,top,0,0};}
            ++layout.line_count;start=end;continue;
        }
        const float baseline=layout_add(top,line_baseline);float metric=0,ink_metric=0,x=0,metric_done=0,metric_part=0;
        u32 trailing=end;while(trailing>start&&(sink.trailing_reset.count?sink.trailing_reset[trailing-1]!=0:(clusters[trailing-1].flags&(native_paragraph_space|native_paragraph_newline|native_paragraph_crlf_prefix))!=0))--trailing;
        u32 at=start;
        while(at<end){const auto& first=clusters[at];const bool control=(first.flags&(native_paragraph_tab|native_paragraph_newline|native_paragraph_nonvisual|native_paragraph_crlf_prefix))!=0;
            const u32 style=first.flags>>native_paragraph_style_shift;
            const bool split_space=at>start&&(first.flags&native_paragraph_space)&&sink.split_leading_space&&sink.split_leading_space(sink.context,style);
            u32 last=at+1;if(!control)while(last<end&&!(clusters[last].flags&(native_paragraph_tab|native_paragraph_newline|native_paragraph_nonvisual|native_paragraph_crlf_prefix))&&last!=trailing&&(!styles.count||(clusters[last].flags>>native_paragraph_style_shift)==style)&&
                (!split_space||(clusters[last].flags&native_paragraph_space)))++last;
            else if(first.flags&native_paragraph_tab)while(last<end&&(clusters[last].flags&native_paragraph_tab)&&(!styles.count||(clusters[last].flags>>native_paragraph_style_shift)==style))++last;
            else if(first.flags&native_paragraph_nonvisual)while(last<end&&(clusters[last].flags&native_paragraph_nonvisual)&&!(clusters[last].flags&native_paragraph_metric_group)&&(!styles.count||(clusters[last].flags>>native_paragraph_style_shift)==style))++last;
            else if((first.flags&native_paragraph_crlf_prefix)&&last<end&&(!styles.count||(clusters[last].flags>>native_paragraph_style_shift)==style))++last;
            if(layout.run_count==runs.count)return SfntStatus::output_too_small;
            // A run is placed relative to its own font baseline, then drawn from that
            // origin; keep the float round trip.
            const float run_baseline=styles.count?layout_add(layout_add(baseline,-styles[style].baseline),styles[style].baseline):baseline;
            const auto& final=clusters[last-1];runs[layout.run_count++]={static_cast<u16>(first.source_first),static_cast<u16>(final.source_end-first.source_first),first.glyph_first,final.glyph_first+final.glyph_count-first.glyph_first,x,run_baseline,0};
            float group_pen=0,tab_pen=metric,point_pen=x;
            for(u32 c=at;c<last;++c){const auto& item=clusters[c];
                if(nominal_groups&&(item.flags&native_paragraph_metric_group)){metric_done=layout_add(metric_done,metric_part);metric_part=0;}
                if(item.glyph_first>placements.count||item.glyph_count>placements.count-item.glyph_first)return SfntStatus::invalid_input;
                const float hit_left=layout_add(x,group_pen);float hit_width=item.width;const u32 advance_first=advance_count;
                if(hits){const u32 needed=(item.flags&native_paragraph_tab)?1u:item.glyph_count;
                    if(needed>native_hit_advance_mask-advance_count)return SfntStatus::resource_limit;
                    if(needed>hits->advances.count-advance_count)return SfntStatus::output_too_small;}
                if(item.flags&native_paragraph_tab){const float next=tab(tab_pen,stop);if(next<0)return SfntStatus::resource_limit;const float delta=next-tab_pen;metric=layout_add(metric,delta);metric_done=metric;metric_part=0;group_pen=layout_add(group_pen,delta);tab_pen=next;ink_metric=metric;hit_width=delta;if(hits)hits->advances[advance_count++]=delta;}
                else if(!(item.flags&(native_paragraph_newline|native_paragraph_crlf_prefix))){
                    if(nominal_groups){metric_part=layout_add(metric_part,item.width);metric=layout_add(metric_done,metric_part);}else metric=layout_add(metric,item.width);
                    if(!(item.flags&native_paragraph_space))ink_metric=metric;
                    for(u32 g=0;g<item.glyph_count;++g){const float advance=placements[item.glyph_first+g].advance;group_pen=layout_add(group_pen,advance);if(hits)hits->advances[advance_count++]=advance;}
                }
                if(hits){const bool newline=(item.flags&(native_paragraph_newline|native_paragraph_crlf_prefix))!=0;
                    const bool atomic=(item.flags&(native_paragraph_atomic_newline|native_paragraph_crlf_prefix))!=0;const u32 count=newline&&!atomic?item.source_end-item.source_first:1u;
                    if(count>hits->clusters.count-hit_count)return SfntStatus::output_too_small;
                    point_pen=layout_add(point_pen,hit_width);
                    for(u32 unit=0;unit<count;++unit)hits->clusters[hit_count++]={static_cast<u16>(item.source_first+unit),static_cast<u16>(newline&&!atomic?item.source_first+unit+1:item.source_end),
                        advance_first|(c==at&&unit==0?native_hit_group_start:0)|(newline?native_hit_newline:0),point_pen,hit_left,hit_width,style};}
            }
            runs[layout.run_count-1].width=group_pen;x=layout_add(x,group_pen);if(!is_finite(x))return SfntStatus::resource_limit;at=last;
        }
        if(fit.data){fit[layout.line_count].width=ink_metric;fit[layout.line_count].extent=metric;}
        if(hits){if(layout.line_count==hits->lines.count)return SfntStatus::output_too_small;hits->lines[layout.line_count]={static_cast<u16>(hit_first),static_cast<u16>(hit_count-hit_first),top,x,static_cast<u16>(ascent_style),static_cast<u16>(descent_style)};}
        if(hits&&hits->alignment_widths.data){if(layout.line_count>=hits->alignment_widths.count)return SfntStatus::output_too_small;hits->alignment_widths[layout.line_count]=ink_metric;}
        if(lines.data){if(layout.line_count==lines.count)return SfntStatus::output_too_small;
            lines[layout.line_count]={static_cast<u16>(run_first),static_cast<u16>(layout.run_count-run_first),top,line_height,x};}
        if(sink.emit){const u32 trailing_source=trailing<end?clusters[trailing].source_first:end?clusters[end-1].source_end:0;
            const auto status=sink.emit(sink.context,{runs.data?runs.data+run_first:nullptr,layout.run_count-run_first},run_first,trailing_source);if(status!=SfntStatus::ok)return status;}
        layout.width=maximum(layout.width,ink_metric);layout.extent=maximum(layout.extent,metric);++layout.line_count;top=layout_add(top,line_height);start=end;
        if(!is_finite(top)||!is_finite(layout.extent))return SfntStatus::resource_limit;
    }while(start<clusters.count);
    if(clusters.count&&(clusters[clusters.count-1].flags&native_paragraph_newline)){
        if(hits){if(layout.line_count==hits->lines.count)return SfntStatus::output_too_small;const u16 style=styles.count?static_cast<u16>(clusters[clusters.count-1].flags>>native_paragraph_style_shift):0;hits->lines[layout.line_count]={static_cast<u16>(hit_count),0,top,0,style,style};}
        if(hits&&hits->alignment_widths.data){if(layout.line_count>=hits->alignment_widths.count)return SfntStatus::output_too_small;hits->alignment_widths[layout.line_count]=0;}
        float height=shape.vertical.height;if(styles.count){const auto& s=styles[clusters[clusters.count-1].flags>>native_paragraph_style_shift];height=layout_add(s.baseline,s.descent);}
        if(trimming_height&&(hidden||layout_add(top,height)>trimming_height)){height=0;layout.flags|=native_paragraph_height_trimmed;}
        if(fit.data){if(layout.line_count==fit.count)return SfntStatus::output_too_small;
            const float baseline=styles.count?styles[clusters[clusters.count-1].flags>>native_paragraph_style_shift].baseline:shape.vertical.baseline;
            fit[layout.line_count]={static_cast<u16>(clusters.count),0,top,height,height?baseline:0,0,0,0,static_cast<u16>(height?0u:native_line_fit_hidden)};}
        if(lines.data){if(layout.line_count==lines.count)return SfntStatus::output_too_small;lines[layout.line_count]={static_cast<u16>(layout.run_count),0,top,height,0};}
        if(sink.emit&&height){const auto status=sink.emit(sink.context,{},layout.run_count,shape.source_units);if(status!=SfntStatus::ok)return status;}
        ++layout.line_count;top=layout_add(top,height);}
    if(!is_finite(top))return SfntStatus::resource_limit;
    if(hits){if(styles.count)copy_bytes(hits->styles.data,styles.data,styles.count*sizeof(SfntFontLineParts));
        NativeParagraphHits result{{hits->clusters.data,hit_count},{hits->lines.data,layout.line_count},{hits->advances.data,advance_count},shape.source_units,styles.count,{}};
        if(styles.count)result.styles=hits->styles.data;else result.uniform=shape.vertical;*hits->output=result;}
    layout.height=top;*output=layout;return SfntStatus::ok;
}
}
SfntStatus native_prepared_paragraph_layout(const NativeParagraphShape& shape,ConstSlice<NativeParagraphCluster> clusters,
    ConstSlice<SfntPlacement> placements,float width,float stop,bool wrap,Slice<NativeParagraphRun> runs,NativeParagraphLayout* output,NativeParagraphHitStorage* hits,float trimming_height,Slice<NativeParagraphLineFit> fit) noexcept {
    return paragraph_layout(shape,clusters,placements,width,stop,wrap,runs,output,hits,{},{},{},trimming_height,fit);
}
SfntStatus native_prepared_styled_paragraph_layout(const NativeParagraphShape& shape,ConstSlice<NativeParagraphCluster> clusters,
    ConstSlice<SfntPlacement> placements,ConstSlice<SfntFontLineParts> styles,float width,float stop,bool wrap,Slice<NativeParagraphRun> runs,NativeParagraphLayout* output,Slice<NativeParagraphLine> lines,NativeParagraphHitStorage* hits,NativeParagraphLineSink sink,float trimming_height,Slice<NativeParagraphLineFit> fit) noexcept {
    if(!styles.count){if(output)reset_value(output);return SfntStatus::invalid_input;}
    return paragraph_layout(shape,clusters,placements,width,stop,wrap,runs,output,hits,styles,lines,sink,trimming_height,fit);
}
}
