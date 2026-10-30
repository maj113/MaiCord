namespace {
constexpr u32 native_decoration_flags=text_style_underline|text_style_strikethrough|text_style_monospace|text_style_code_block|text_style_spoiler|text_style_mention|text_style_quote|text_style_list_bullet;
float decoration_add(float a,float b) noexcept {return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a),_mm_set_ss(b)));}
float decoration_sub(float a,float b) noexcept {return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(a),_mm_set_ss(b)));}
float decoration_mul(float a,float b) noexcept {return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(a),_mm_set_ss(b)));}
float decoration_div(float a,float b) noexcept {return _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(a),_mm_set_ss(b)));}
float decoration_width(const NativeTextContext& owner,u32 run,const ShapedText* drawn) noexcept {
    if(!drawn||!drawn->runs[run].glyph_count)return owner.paragraph_runs[run].width;
    const auto& r=drawn->runs[run];float width=0;for(u32 g=0;g<r.glyph_count;++g)width=decoration_add(width,drawn->glyphs[r.first_glyph+g].advance);return width;
}
bool decoration_inverse_order(ShapedText& drawn,u32 first,u32 count) noexcept {
    // The line publisher stores logical->visual ordinals in unpublished source
    // fields. Invert that permutation by cycles, without a second index array.
    constexpr u32 visited=0x80000000u;
    for(u32 start=0;start<count;++start){if(drawn.runs[first+start].text_position&visited)continue;
        u32 current=start,next=drawn.runs[first+start].text_position;
        while(next!=start){if(next>=count||(drawn.runs[first+next].text_position&visited))return false;
            const u32 following=drawn.runs[first+next].text_position;drawn.runs[first+next].text_position=current|visited;current=next;next=following;}
        drawn.runs[first+start].text_position=current|visited;
    }
    for(u32 i=0;i<count;++i)drawn.runs[first+i].text_position&=~visited;return true;
}
u32 decoration_span(const NativeTextContext& owner,u32 source) noexcept {
    u32 lo=0,hi=owner.styled_spans.count;while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(owner.styled_spans[mid].end_unit<=source)lo=mid+1;else hi=mid;}return lo;
}
u32 decoration_paint(const NativeTextContext& owner,u32 span) noexcept {
    const u32 byte=owner.styled_spans[span].first_byte;u32 lo=0,hi=owner.last_styles.count;
    while(lo<hi){const u32 mid=lo+(hi-lo)/2;const auto& style=owner.last_styles[mid];if(style.byte_offset+style.byte_count<=byte)lo=mid+1;else hi=mid;}
    return lo<owner.last_styles.count&&owner.last_styles[lo].byte_offset<=byte?lo+1:0;
}
// An underline/line-through quad; under a ```ansi run it takes that run's foreground.
TextDecoration decoration_line(const NativeTextContext& owner,u32 span,float left,float top,float right,float bottom,TextDecorationKind kind) noexcept {
    TextDecoration decoration{left,top,right,bottom,owner.styled_spans[span].flags,kind,0,0,0};const u32 paint=decoration_paint(owner,span);
    if(paint)ansi_decoration_set_line_color(decoration,ansi_span_foreground(owner.last_styles[paint-1]));return decoration;
}
u32 decoration_line_end(const NativeTextContext& owner,const NativeParagraphLine& line) noexcept {
    u32 end=static_cast<u32>(line.first_run)+line.run_count;
    while(end>line.first_run){const auto& run=owner.paragraph_runs[end-1];u32 lo=0,hi=owner.paragraph_shape.cluster_count;
        while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(owner.paragraph_storage.clusters[mid].source_end<=run.source_first)lo=mid+1;else hi=mid;}
        bool blank=true;for(u32 c=lo;c<owner.paragraph_shape.cluster_count&&owner.paragraph_storage.clusters[c].source_first<static_cast<u32>(run.source_first)+run.source_count;++c)
            blank=blank&&(owner.paragraph_storage.clusters[c].flags&(native_paragraph_space|native_paragraph_newline|native_paragraph_crlf_prefix))!=0;
        if(!blank)break;--end;
    }return end;
}
struct DecorationGraphemeCursor {UnicodeGraphemeState state{};u32 byte=0,source=0;};
u8 decoration_terminal_boundary(const NativeTextContext& owner,u32 source,DecorationGraphemeCursor& cursor) noexcept {
    if(source<cursor.source)cursor={};
    while(cursor.source<source){const auto cp=utf8_decode_one(owner.last_text.data+cursor.byte,owner.last_text.count-cursor.byte);
        if(!cp.valid)return false;unicode_grapheme_boundary(cursor.state,cp.codepoint);cursor.byte+=cp.width;cursor.source+=cp.codepoint>65535?2u:1u;}
    if(cursor.source!=source)return false;bool control=false;
    if(cursor.byte){u32 first=cursor.byte-1;while(first&&(static_cast<u8>(owner.last_text[first])&0xc0)==0x80)--first;
        const auto previous=utf8_decode_one(owner.last_text.data+first,cursor.byte-first);if(!previous.valid)return false;control=previous.codepoint==0x200c||previous.codepoint==0x200d||previous.codepoint==0x34f;}
    if(cursor.byte==owner.last_text.count)return control?2:1;
    const auto cp=utf8_decode_one(owner.last_text.data+cursor.byte,owner.last_text.count-cursor.byte);auto state=cursor.state;
    return cp.valid&&unicode_grapheme_boundary(state,cp.codepoint)?static_cast<u8>(control?2:1):0;
}
struct NativeStyledRangeSink {
    void* context;bool (*emit)(void*,const NativeParagraphLine&,float,float) noexcept;
    bool operator()(const NativeParagraphLine& line,float left,float width) const noexcept {return emit(context,line,left,width);}
};
struct NativeOmissionZero {u32 first,end;float x;u8 level;};
struct NativeStyledOmission {u32 source;float x,width,height;u32 visual_before;ConstSlice<NativeOmissionZero> zeroes;u32 line=0,end_source=0xffffffffu;};
// One walker serves semantic regions and both decoration visitors. Keep the
// substantial bidi/cluster logic out of each callback's template instantiation.
__declspec(noinline) bool native_styled_range(const NativeTextContext& owner,const NativeParagraphLayout& layout,float tab_stop,u32 first,u32 end,NativeStyledRangeSink visit,DecorationGraphemeCursor* boundaries,const ShapedText* drawn,const NativeStyledOmission* omission=nullptr,ConstSlice<NativeStyledOmission> omissions={}) noexcept {
    if(omissions.count){
        u32 cursor=first;
        for(u32 i=0;i<omissions.count;++i){const auto& item=omissions[i];
            if(end<=item.source)break;if(cursor>=item.end_source)continue;
            if(cursor<item.source&&!native_styled_range(owner,layout,tab_stop,cursor,minimum(end,item.source),visit,boundaries,drawn))return false;
            const u32 begin=maximum(cursor,item.source),limit=minimum(end,item.end_source);
            if(begin<limit){const NativeParagraphLine line{0,0,owner.styled_lines[item.line].top,item.height,item.width};
                if(begin==item.source&&!visit(line,item.x,item.width))return false;
                for(u32 z=0;z<item.zeroes.count;++z){const auto& zero=item.zeroes[z];if(begin<zero.end&&limit>zero.first&&!visit(line,zero.x,0))return false;}}
            cursor=maximum(cursor,item.end_source);
        }
        return cursor>=end||native_styled_range(owner,layout,tab_stop,cursor,end,visit,boundaries,drawn);
    }
    if(omission){
        if(first>end||end>owner.paragraph_shape.source_units)return false;
        if(first<omission->source&&layout.run_count&&
           !native_styled_range(owner,layout,tab_stop,first,minimum(end,omission->source),visit,boundaries,drawn))return false;
        const NativeParagraphLine line{0,0,0,omission->height,omission->width};
        if(end<=omission->source)return true;
        if(first<=omission->source&&!visit(line,omission->x,omission->width))return false;
        for(u32 i=0;i<omission->zeroes.count;++i){const auto& zero=omission->zeroes[i];
            if(first<zero.end&&end>zero.first&&!visit(line,zero.x,0))return false;}
        return true;
    }
    // An omission can replace every ordinary run. The sign was handled above;
    // a remaining hidden source range has no positive-area geometry to emit.
    if(!layout.run_count)return true;
    if(owner.last_mode&48){
        if(first>end||end>owner.paragraph_shape.source_units||!drawn||!boundaries)return false;
        const auto floor_source=[&](u32 source) noexcept {u32 lo=0,hi=owner.paragraph_shape.cluster_count;
            while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(owner.paragraph_storage.clusters[mid].source_end<=source)lo=mid+1;else hi=mid;}
            return lo<owner.paragraph_shape.cluster_count?owner.paragraph_storage.clusters[lo].source_first:owner.paragraph_shape.source_units;};
        // A paint cut inside a complete grapheme keeps an empty end region for
        // RTL, emoji or zero-advance clusters; a fragment of a larger grapheme does not.
        const u32 requested_end=end;first=floor_source(first);end=floor_source(end);bool terminal_point=false,terminal_zero=false;
        if(requested_end!=end){u32 lo=0,hi=owner.paragraph_shape.cluster_count;
            while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(owner.paragraph_storage.clusters[mid].source_end<=end)lo=mid+1;else hi=mid;}
            if(lo<owner.paragraph_shape.cluster_count){const auto& cluster=owner.paragraph_storage.clusters[lo];const u8 boundary=decoration_terminal_boundary(owner,cluster.source_end,*boundaries);
                terminal_point=boundary==1;terminal_zero=cluster.width==0;
                const u32 span=cluster.flags>>native_paragraph_style_shift;
                if(boundary==1&&owner.styled_spans[span].binding()->font_flags==emoji_font_flag)terminal_zero=true;
                if(boundary==2&&(cluster.flags&native_paragraph_space)&&span&&owner.styled_spans[span].first_unit==end){const auto& a=owner.styled_spans[span-1];const auto& b=owner.styled_spans[span];
                    if(a.binding()->plan.slot!=b.binding()->plan.slot||a.binding()->plan.generation!=b.binding()->plan.generation||a.em!=b.em||((a.binding()->key^b.binding()->key)&0x0c000000u)||
                       ((owner.last_mode&32)&&((owner.styled_analysis[span-1]^owner.styled_analysis[span])&255)))terminal_point=terminal_zero=true;}
            }}
        u32 first_run=0,first_line=0;
        if(first){u32 lo=0,hi=layout.run_count;
            while(lo<hi){const u32 mid=lo+(hi-lo)/2;const auto& r=owner.paragraph_runs[mid];if(static_cast<u32>(r.source_first)+r.source_count<=first)lo=mid+1;else hi=mid;}first_run=lo;
            lo=0;hi=layout.line_count;while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(owner.styled_lines[mid].first_run<=first_run)lo=mid+1;else hi=mid;}first_line=lo?lo-1:0;}
        if(first==end){
            for(u32 line=first_line;line<layout.line_count;++line){const auto& l=owner.styled_lines[line];const u32 limit=static_cast<u32>(l.first_run)+l.run_count;
                for(u32 index=maximum(first_run,static_cast<u32>(l.first_run));index<limit;++index){const auto& source=owner.paragraph_runs[index];
                    if(static_cast<u32>(source.source_first)+source.source_count<=first)continue;
                    const auto& r=drawn->runs[index];float offset=0;
                    if(r.glyph_count){const u32 glyph=drawn->clusters[first];if(glyph<r.first_glyph||glyph>r.first_glyph+r.glyph_count)return false;
                        const u32 begin=(r.bidi_level&1)?glyph:r.first_glyph,last=(r.bidi_level&1)?r.first_glyph+r.glyph_count:glyph;
                        for(u32 g=begin;g<last;++g)offset=decoration_add(offset,drawn->glyphs[g].advance);}
                    return visit(l,decoration_add(r.baseline_y,offset),0.f);
                }
                if(line+1==layout.line_count)return visit(l,l.extent,0.f);
            }return true;
        }
        for(u32 line=first_line;line<layout.line_count;++line){const auto& l=owner.styled_lines[line];const u32 limit=static_cast<u32>(l.first_run)+l.run_count;
            for(u32 run=maximum(first_run,static_cast<u32>(l.first_run));run<limit;){const auto& source=owner.paragraph_runs[run];
                if(static_cast<u32>(source.source_first)+source.source_count<=first){++run;continue;}if(source.source_first>end||(source.source_first==end&&(!terminal_point||(!terminal_zero&&!(drawn->runs[run].bidi_level&1)))))break;
                const u8 level=drawn->runs[run].bidi_level;float left=0,width=0;u32 next=run;
                do{const auto& r=drawn->runs[next];const auto& origin=owner.paragraph_runs[next];
                    const u32 begin=maximum(first,static_cast<u32>(origin.source_first)),last=minimum(end,static_cast<u32>(origin.source_first)+origin.source_count);
                    u32 first_glyph=r.first_glyph,last_glyph=r.first_glyph+r.glyph_count;
                    if(r.glyph_count&&begin!=origin.source_first)first_glyph=drawn->clusters[begin];
                    if(r.glyph_count&&last!=static_cast<u32>(origin.source_first)+origin.source_count)last_glyph=drawn->clusters[last];
                    if(first_glyph<r.first_glyph||last_glyph<first_glyph||last_glyph>r.first_glyph+r.glyph_count)return false;
                    float advance=r.glyph_count?0:origin.width;for(u32 g=first_glyph;g<last_glyph;++g)advance=decoration_add(advance,drawn->glyphs[g].advance);
                    const u32 offset_first=r.first_glyph,offset_end=first_glyph;
                    float offset=0;for(u32 g=offset_first;g<offset_end;++g)offset=decoration_add(offset,drawn->glyphs[g].advance);
                    const bool partial=first_glyph!=r.first_glyph||last_glyph!=r.first_glyph+r.glyph_count;
                    const float edge=(level&1)&&partial?decoration_sub(decoration_sub(r.baseline_x,offset),advance):decoration_add(r.baseline_y,offset);
                    left=next==run?edge:minimum(left,edge);width=decoration_add(width,advance);++next;
                }while(next<limit&&(owner.paragraph_runs[next].source_first<end||(terminal_point&&(terminal_zero||(drawn->runs[next].bidi_level&1))&&owner.paragraph_runs[next].source_first==end))&&drawn->runs[next].bidi_level==level);
                if(!visit(l,left,width))return false;run=next;}
            if(limit){const auto& last=owner.paragraph_runs[limit-1];const u32 line_end=static_cast<u32>(last.source_first)+last.source_count;
                if(line_end>end||(line_end==end&&!terminal_point))break;}
        }return true;
    }
    const auto& shape=owner.paragraph_shape;const auto& s=owner.paragraph_storage;
    const NativeParagraphGeometry g{{s.clusters.data,shape.cluster_count},{s.placements.data,shape.glyph_count},{owner.paragraph_runs.data,layout.run_count},{owner.styled_lines.data,layout.line_count},shape.source_units,tab_stop};
    bool terminal=false;
    if(boundaries){u32 lo=0,hi=shape.cluster_count;while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(s.clusters[mid].source_end<=end)lo=mid+1;else hi=mid;}
        if(lo<shape.cluster_count&&s.clusters[lo].source_first!=end){const auto& cluster=s.clusters[lo];const auto& span=owner.styled_spans[cluster.flags>>native_paragraph_style_shift];
            terminal=span.binding()->font_flags==emoji_font_flag&&decoration_terminal_boundary(owner,cluster.source_end,*boundaries)==1;}}
    u32 count=0;return native_paragraph_geometry_visit(g,first,end-first,{&visit,[](void* context,const NativeParagraphHitMetric& metric) noexcept {
        const NativeParagraphLine line{0,0,metric.top,metric.height,0};return (*static_cast<NativeStyledRangeSink*>(context))(line,metric.left,metric.width);}},&count,terminal)==SfntStatus::ok;
}
template<class Visitor> bool decoration_range(const NativeTextContext& owner,const NativeParagraphLayout& layout,float tab_stop,u32 first,u32 end,Visitor visit,DecorationGraphemeCursor* boundaries=nullptr,const ShapedText* drawn=nullptr,const NativeStyledOmission* omission=nullptr,ConstSlice<NativeStyledOmission> omissions={}) noexcept {
    return native_styled_range(owner,layout,tab_stop,first,end,{&visit,[](void* context,const NativeParagraphLine& line,float left,float width) noexcept {
        return (*static_cast<Visitor*>(context))(line,left,width);}},boundaries,drawn,omission,omissions);
}
// One hit-region emitter for every publisher. `grapheme` selects the directional
// grapheme-boundary cursor; the other arguments pass straight to native_styled_range.
__declspec(noinline) bool native_collect_hit_regions(const NativeTextContext& owner,const NativeParagraphLayout& layout,float tab_stop,ConstSlice<TextHitRange> hits,bool grapheme,
    ShapedText* output,u32* count,const ShapedText* drawn,const NativeStyledOmission* omission=nullptr,ConstSlice<NativeStyledOmission> omissions={}) noexcept {
    struct Sink {ShapedText* output;u32 count;const TextHitRange* hit;} sink{output,0,nullptr};
    DecorationGraphemeCursor boundaries{};const u32 units=owner.paragraph_shape.source_units;
    for(u32 h=0;h<hits.count;++h){const auto& hit=hits[h];
        if(!hit.unit_count||hit.first_unit>units||hit.unit_count>units-hit.first_unit)return false;
        sink.hit=&hit;
        if(!native_styled_range(owner,layout,tab_stop,hit.first_unit,hit.first_unit+hit.unit_count,{&sink,[](void* context,const NativeParagraphLine& line,float left,float width) noexcept {
            auto& s=*static_cast<Sink*>(context);
            if(!(width>0)||!(line.height>0))return true;
            if(!s.output->hit_regions||s.count==s.output->hit_region_capacity)return false;
            s.output->hit_regions[s.count++]={left,line.top,decoration_add(left,width),decoration_add(line.top,line.height),s.hit->semantic_index,0};return true;}},
            grapheme?&boundaries:nullptr,drawn,omission,omissions))return false;
    }
    *count=sink.count;return true;
}
struct NativeDecorationPoint {float left,top,width,height;};
bool native_decoration_point(const NativeTextContext& owner,const NativeParagraphLayout& layout,float tab_stop,u32 source,
    const ShapedText* drawn,ConstSlice<NativeStyledOmission> omissions,NativeDecorationPoint* output) noexcept {
    u32 last_line=0,last_source=0;
    for(u32 i=0;i<layout.line_count;++i){const auto& line=owner.styled_lines[i];if(!(line.height>0))continue;last_line=i;
        if(line.run_count){const auto& run=owner.paragraph_runs[line.first_run+line.run_count-1];last_source=static_cast<u32>(run.source_first)+run.source_count;}}
    for(u32 i=0;i<omissions.count;++i){const auto& item=omissions[i];if(source>=item.source&&(source<item.end_source||(item.line==last_line&&source>=last_source))){
        *output={item.x,owner.styled_lines[item.line].top,item.width,item.height};return true;}}
    if(!last_source)return false;source=minimum(source,last_source-1);bool found=false;DecorationGraphemeCursor boundaries{};
    if(!decoration_range(owner,layout,tab_stop,source,source+1,[&](const NativeParagraphLine& line,float left,float width) noexcept {
        if(!found&&line.height>0){*output={left,line.top,width,line.height};found=true;}return true;
    },&boundaries,drawn))return false;return found;
}
bool native_styled_decorations(const NativeTextContext& owner,const NativeParagraphLayout& layout,float maximum_width,float tab_stop,Slice<TextDecoration> output,u32* count,ShapedText* drawn=nullptr,const NativeStyledOmission* omission=nullptr,ConstSlice<NativeStyledOmission> omissions={}) noexcept {
    *count=0;u32 written=0;DecorationGraphemeCursor boundaries{};
    const auto append=[&](TextDecoration value) noexcept {if(written==output.count||!output.data)return false;output[written++]=value;return true;};
    bool underlined=false;for(u32 i=0;i<owner.last_styles.count;++i)underlined|=(owner.last_styles[i].flags&text_style_underline)!=0;
    if(underlined)for(u32 line=0;line<layout.line_count;++line){
        const NativeStyledOmission* active=omission;for(u32 i=0;i<omissions.count;++i)if(omissions[i].line==line){active=omissions.data+i;break;}
        const auto& l=owner.styled_lines[line];const u32 end=decoration_line_end(owner,l);
        if(drawn&&!decoration_inverse_order(*drawn,l.first_run,l.run_count))return false;
        const auto run_at=[&](u32 visual) noexcept {return static_cast<u32>(l.first_run)+(drawn?drawn->runs[l.first_run+visual].text_position:visual);};
        for(u32 visual=0;visual<l.run_count;){const u32 run=run_at(visual),style=decoration_span(owner,owner.paragraph_runs[run].source_first);
            if(run>=end||!(owner.styled_spans[style].flags&text_style_underline)){++visual;continue;}
            u32 last=visual;float width=0,offset=0,thickness=0,baseline=0;
            while(last<l.run_count){const u32 index=run_at(last);const auto& r=owner.paragraph_runs[index];const auto& span=owner.styled_spans[decoration_span(owner,r.source_first)];
                if(active&&last!=visual&&last==active->visual_before)break;
                if(index>=end||!(span.flags&text_style_underline))break;
                const auto* m=native_run_plan_metrics(owner.store,span.binding()->plan);if(!m)return false;const float advance=decoration_width(owner,index,drawn);
                const float position=decoration_div(decoration_mul(static_cast<float>(m->underline_position),span.em),static_cast<float>(m->units_per_em));
                const float size=decoration_div(decoration_mul(static_cast<float>(m->underline_thickness),span.em),static_cast<float>(m->units_per_em));
                offset=decoration_add(offset,-decoration_mul(position,advance));thickness=decoration_add(thickness,decoration_mul(size,advance));width=decoration_add(width,advance);baseline=maximum(baseline,r.baseline);++last;}
            if(width>0){offset=decoration_div(offset,width);thickness=decoration_div(thickness,width);const float top=decoration_add(baseline,offset);
                for(u32 part=visual;part<last;){const u32 selected_run=run_at(part),selected=decoration_span(owner,owner.paragraph_runs[selected_run].source_first);
                    const float left=drawn?drawn->runs[selected_run].baseline_y:owner.paragraph_runs[selected_run].x;float extent=0;u32 next=part;
                    do{extent=decoration_add(extent,decoration_width(owner,run_at(next),drawn));++next;}while(next<last&&(!drawn||((drawn->runs[run_at(next)].bidi_level^drawn->runs[selected_run].bidi_level)&1)==0)&&decoration_paint(owner,decoration_span(owner,owner.paragraph_runs[run_at(next)].source_first))==decoration_paint(owner,selected));
                    const float right=decoration_add(left,extent),bottom=decoration_add(top,thickness);
                    if(extent!=0&&!append(decoration_line(owner,selected,left,top,right,bottom,TextDecorationKind::underline)))return false;
                    part=next;}
            }visual=last;
        }
    }
    for(u32 line=0;line<layout.line_count;++line){const auto& l=owner.styled_lines[line];const u32 end=decoration_line_end(owner,l);
    for(u32 run=l.first_run;run<end;++run){const auto& r=owner.paragraph_runs[run];const auto& span=owner.styled_spans[decoration_span(owner,r.source_first)];if(!(span.flags&text_style_strikethrough))continue;
        const float width=decoration_width(owner,run,drawn);if(!(width>0))continue;const auto* m=native_run_plan_metrics(owner.store,span.binding()->plan);if(!m)return false;
        const float offset=decoration_div(decoration_mul(-static_cast<float>(m->strike_position),span.em),static_cast<float>(m->units_per_em));
        const float thickness=decoration_div(decoration_mul(static_cast<float>(m->strike_thickness),span.em),static_cast<float>(m->units_per_em));const float top=decoration_add(r.baseline,offset);
        const float left=drawn?drawn->runs[run].baseline_y:r.x;
        if(!append(decoration_line(owner,decoration_span(owner,r.source_first),left,top,decoration_add(left,width),decoration_add(top,thickness),TextDecorationKind::strikethrough)))return false;
    }
    }
    for(u32 span=0;span<owner.styled_spans.count;){const auto& s=owner.styled_spans[span];if(!(s.flags&text_style_code_block)){++span;continue;}
        u32 next=span+1;while(next<owner.styled_spans.count&&(owner.styled_spans[next].flags&text_style_code_block))++next;
        // A framed block's box is drawn by its owner.
        if(const u32 paint=decoration_paint(owner,span);paint&&text_span_framed_block(owner.last_styles[paint-1])){span=next;continue;}
        bool bounded=false,multiline=false;float left=0,top=0,right=0,bottom=0;
        if(!decoration_range(owner,layout,tab_stop,s.first_unit,owner.styled_spans[next-1].end_unit,[&](const NativeParagraphLine& l,float x,float width) noexcept {
            if(!(l.height>0))return true;
            const float end=decoration_add(x,width),base=decoration_add(l.top,l.height);
            if(!bounded){left=x;top=l.top;right=end;bottom=base;bounded=true;}
            else{multiline|=l.top!=top;left=minimum(left,x);top=minimum(top,l.top);right=maximum(right,end);bottom=maximum(bottom,base);}return true;},&boundaries,drawn,omission,omissions))return false;
        if(multiline){left=0;right=maximum_width;}
        if(!bounded){NativeDecorationPoint first{},last{};
            if(!native_decoration_point(owner,layout,tab_stop,s.first_unit,drawn,omissions,&first)||
               !native_decoration_point(owner,layout,tab_stop,owner.styled_spans[next-1].end_unit-1,drawn,omissions,&last))return false;
            left=minimum(first.left,last.left);top=minimum(first.top,last.top);right=maximum(decoration_add(first.left,first.width),decoration_add(last.left,last.width));bottom=maximum(decoration_add(first.top,first.height),decoration_add(last.top,last.height));
            if(last.top>first.top){left=0;right=maximum_width;}bounded=bottom>top;
        }
        if(bounded&&!append({maximum(0.f,left-4),top-2,minimum(maximum_width,right+4),bottom+2,text_style_monospace|text_style_code_block,TextDecorationKind::background,0,0,0}))return false;span=next;
    }
    // One quote spine per run of quoted paint ranges over their line boxes,
    // 16 DIP left of the text.
    for(u32 paint=0,quote_span=0;paint<owner.last_styles.count;){const auto& input=owner.last_styles[paint];
        if(!(input.flags&text_style_quote)){++paint;continue;}
        u32 end=input.byte_offset+input.byte_count,next=paint+1;
        while(next<owner.last_styles.count&&(owner.last_styles[next].flags&text_style_quote)&&owner.last_styles[next].byte_offset==end){end+=owner.last_styles[next].byte_count;++next;}
        while(quote_span<owner.styled_spans.count&&owner.styled_spans[quote_span].end_byte<=input.byte_offset)++quote_span;
        if(quote_span==owner.styled_spans.count)return false;u32 last=quote_span;
        while(last+1<owner.styled_spans.count&&owner.styled_spans[last].end_byte<end)++last;
        NativeDecorationPoint first{},final{};
        if(!native_decoration_point(owner,layout,tab_stop,owner.styled_spans[quote_span].first_unit,drawn,omissions,&first)||
           !native_decoration_point(owner,layout,tab_stop,owner.styled_spans[last].end_unit-1,drawn,omissions,&final))return false;
        const float bottom=maximum(decoration_add(first.top,first.height),decoration_add(final.top,final.height));
        if(bottom>first.top&&!append({-16.f,first.top,-12.f,bottom,input.flags,TextDecorationKind::quote_bar,0,0,0}))return false;
        paint=next;}
    // Backgrounds and line markers belong to input paint ranges. Internal
    // emoji/font cuts must not introduce extra padding or duplicate markers.
    u32 span=0;
    for(u32 paint=0;paint<owner.last_styles.count;++paint){const auto& input=owner.last_styles[paint];
        while(span<owner.styled_spans.count&&owner.styled_spans[span].end_byte<=input.byte_offset)++span;
        if(span==owner.styled_spans.count)return false;auto s=owner.styled_spans[span];
        while(span+1<owner.styled_spans.count&&owner.styled_spans[span].end_byte<input.byte_offset+input.byte_count)++span;
        s.end_unit=owner.styled_spans[span].end_unit;constexpr u32 background_flags=text_style_monospace|text_style_spoiler|text_style_mention;
        if(!(s.flags&(background_flags|text_style_list_bullet)))continue;
        const bool begins_line=!s.first_byte||owner.last_text[s.first_byte-1]=='\n';bool bullet=false;
        // A ```ansi background covers each line fragment's line box, like a CSS inline background.
        const u16 ansi_background=ansi_span_background(input);
        if(!decoration_range(owner,layout,tab_stop,s.first_unit,s.end_unit,[&](const NativeParagraphLine& l,float left,float width) noexcept {
            if(!(l.height>0))return true;
            if(ansi_background&&width>0&&!append(ansi_background_decoration(left,l.top,left+width,l.top+l.height,s.flags,ansi_background)))return false;
            if(begins_line&&!bullet&&width>0&&(s.flags&text_style_list_bullet)){const float x=left+5+static_cast<float>((s.flags&kTextStyleListDepthMask)>>kTextStyleListDepthShift)*8;const float y=l.top+l.height*.5f;
                if(!append({x-2,y-2,x+2,y+2,s.flags,TextDecorationKind::list_bullet,0,0,0}))return false;bullet=true;}
            return (s.flags&text_style_code_block)||!(s.flags&background_flags)||!(width>0)||append({left-2,l.top-1,left+width+2,l.top+l.height+1,s.flags,TextDecorationKind::background,0,0,0});
        },&boundaries,drawn,omission,omissions))return false;
    }
    *count=written;return true;
}
}
