#include "maicord/text/paragraph_hits.hpp"
#include "layout_float.hpp"
namespace maicord {namespace {
float sub(float a,float b) noexcept {return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(a),_mm_set_ss(b)));}
bool valid(const NativeParagraphHits& h) noexcept {
    return h.source_units<=65535&&h.clusters.count<=65535&&h.visual_runs.count<=65535&&h.lines.count&&h.lines.data&&(!h.clusters.count||h.clusters.data)&&(!h.advances.count||h.advances.data)&&(!h.visual_runs.count||h.visual_runs.data)&&
        (h.style_count?h.styles!=nullptr:layout_finite(h.uniform.height)&&h.uniform.height>0&&layout_finite(h.uniform.baseline));
}
u32 cluster_at(const NativeParagraphHits& h,u32 source) noexcept {
    u32 first=0,last=h.clusters.count;while(first<last){const u32 middle=first+(last-first)/2;
        if(h.clusters[middle].source_end<=source)first=middle+1;else last=middle;}return first;
}
u32 line_at(const NativeParagraphHits& h,u32 cluster) noexcept {
    u32 first=0,last=h.lines.count;while(first<last){const u32 middle=first+(last-first)/2;
        if(h.lines[middle].first_cluster<=cluster)first=middle+1;else last=middle;}return first?first-1:0;
}
float line_height(const NativeParagraphHits& h,const NativeParagraphHitLine& line) noexcept {
    return h.style_count?layout_add(h.styles[line.ascent_style].baseline,h.styles[line.descent_style].descent):h.uniform.height;
}
const NativeParagraphHitRun* run_for(const NativeParagraphHits& h,u32 cluster) noexcept {
    if(!h.visual_runs.count||cluster>=h.clusters.count)return nullptr;
    const u32 index=h.clusters[cluster].style>>16;if(index>=h.visual_runs.count)return nullptr;
    const auto& run=h.visual_runs[index];return run.first_cluster<=cluster&&cluster<static_cast<u32>(run.first_cluster)+run.cluster_count?&run:nullptr;
}
u32 visual_run_boundary(const NativeParagraphHits& h,u32 cluster) noexcept {
    // This predicate is monotone at a line boundary, even though runs within
    // each line are in visual order rather than source order.
    u32 first=0,last=h.visual_runs.count;while(first<last){const u32 middle=first+(last-first)/2;
        if(h.visual_runs[middle].first_cluster<cluster)first=middle+1;else last=middle;}return first;
}
bool run_part(const NativeParagraphHits& h,const NativeParagraphHitRun& run,u32 first,u32 end,float* left,float* width) noexcept {
    const u32 limit=static_cast<u32>(run.first_cluster)+run.cluster_count;
    if(first<run.first_cluster||end<first||end>limit||limit>h.clusters.count)return false;
    if(first==run.first_cluster&&end==limit){*left=run.left;*width=run.width;return true;}
    *width=0;
    for(u32 c=first;c<end;++c){ConstSlice<float> advances{};if(!native_paragraph_hit_advances(h,c,&advances))return false;
        for(u32 g=0;g<advances.count;++g)*width=layout_add(*width,advances[g]);}
    const float leading=first<limit?h.clusters[first].left:(run.bidi_level&1)?run.left:layout_add(run.left,run.width);
    *left=(run.bidi_level&1)?sub(leading,*width):leading;return true;
}
NativeParagraphHitMetric metric(const NativeParagraphHits& h,u32 c,u32 line) noexcept {
    // Position/point rectangles are baseline-relative. Line lookup and range
    // rectangles use the accumulated top; those can differ by one float ULP.
    const auto& l=h.lines[line];float baseline=0,font_baseline=0,height=0;
    if(h.style_count){baseline=h.styles[l.ascent_style].baseline;const u32 style=(c<h.clusters.count?h.clusters[c].style:h.clusters.count?h.clusters[h.clusters.count-1].style:l.ascent_style)&65535u;
        const auto& parts=h.styles[style];font_baseline=parts.baseline;height=layout_add(parts.baseline,parts.descent);}
    else{baseline=font_baseline=h.uniform.baseline;height=h.uniform.height;}
    const float top=_mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(layout_add(l.top,baseline)),_mm_set_ss(font_baseline)));
    if(c==h.clusters.count){float x=l.caret_end;u32 level=0;
        if(l.cluster_count&&h.visual_runs.count){const auto* run=run_for(h,c-1);if(run){level=run->bidi_level;x=(level&1)?run->left:layout_add(run->left,run->width);}}
        return {static_cast<u16>(h.source_units),0,x,top,0,height,level};}
    const auto& item=h.clusters[c];const auto* run=run_for(h,c);return {item.source_first,static_cast<u16>(item.source_end-item.source_first),run&&(run->bidi_level&1)?sub(item.left,item.width):item.left,top,item.width,height,run?run->bidi_level:0};
}
}
SfntStatus native_paragraph_hit_position(const NativeParagraphHits& h,u32 position,bool trailing,NativeParagraphHitMetric* out,float* x) noexcept {
    if(!out||!x)return SfntStatus::invalid_input;*out={};*x=0;
    if(!valid(h)||position>h.source_units)return SfntStatus::invalid_input;
    const u32 c=cluster_at(h,position);const u32 line=line_at(h,c);if(h.visual_runs.count&&c<h.clusters.count&&!run_for(h,c))return SfntStatus::invalid_input;
    *out=metric(h,c,line);*x=(trailing!=((out->bidi_level&1)!=0))?layout_add(out->left,out->width):out->left;return SfntStatus::ok;
}
SfntStatus native_paragraph_hit_point(const NativeParagraphHits& h,float x,float y,NativeParagraphHitPoint* out,u32* character) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};if(!valid(h)||!layout_finite(x)||!layout_finite(y))return SfntStatus::invalid_input;
    u32 first=0,last=h.lines.count;while(first<last){const u32 middle=first+(last-first)/2;if(h.lines[middle].top<=y)first=middle+1;else last=middle;}
    const u32 line=first?first-1:0;const auto& l=h.lines[line];
    if(l.first_cluster>h.clusters.count||l.cluster_count>h.clusters.count-l.first_cluster)return SfntStatus::invalid_input;
    if(!l.cluster_count){const auto m=metric(h,h.clusters.count,line);out->source_position=h.source_units;if(character)*character=h.source_units;out->inside=x==0&&y>=m.top&&y<=layout_add(m.top,m.height);return SfntStatus::ok;}
    if(h.visual_runs.count){const u32 begin=visual_run_boundary(h,l.first_cluster),end=visual_run_boundary(h,static_cast<u32>(l.first_cluster)+l.cluster_count);
        if(begin==end||end>h.visual_runs.count)return SfntStatus::invalid_input;
        first=begin;last=end;while(first<last){const u32 middle=first+(last-first)/2;const float right=middle+1<end?h.visual_runs[middle+1].left:l.caret_end;
            if(right<=x)first=middle+1;else last=middle;}u32 selected=l.caret_end==0?h.clusters[static_cast<u32>(l.first_cluster)+l.cluster_count-1].style>>16:minimum(first,end-1);
        if(selected<begin||selected>=end)return SfntStatus::invalid_input;
        while(x<0&&l.caret_end>0&&selected+1<end&&!(h.visual_runs[selected].bidi_level&1)&&!h.visual_runs[selected].width&&h.visual_runs[selected+1].left==0)++selected;
        const auto& run=h.visual_runs[selected];const u32 limit=static_cast<u32>(run.first_cluster)+run.cluster_count;const bool rtl=(run.bidi_level&1)!=0;
        if(!run.cluster_count||limit>h.clusters.count)return SfntStatus::invalid_input;
        // Point boundaries retain the run's ordered cluster-width reduction.
        // Rectangle edges instead use ordered glyph advances; substituting
        // those edges changes source affinity beside zero-width controls.
        first=run.first_cluster;last=limit;while(first<last){const u32 middle=first+(last-first)/2;const float edge=h.clusters[middle].point_edge;
            if(rtl?edge>x:edge<=x)first=middle+1;else last=middle;}
        u32 c=minimum(first,limit-1);
        while(c>l.first_cluster&&(h.clusters[c].advance_flags&native_hit_newline)&&(h.clusters[c-1].advance_flags&native_hit_newline))--c;
        const auto& item=h.clusters[c];const auto m=metric(h,c,line);const float half=_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(item.width),_mm_set_ss(.5f)));
        const bool trailing=!(item.advance_flags&native_hit_newline)&&(rtl?x<layout_add(m.left,half):x>=layout_add(m.left,half));
        out->source_position=trailing?item.source_end:item.source_first;if(character)*character=item.source_first;out->inside=x>=0&&x<=l.caret_end&&y>=m.top&&y<=layout_add(m.top,m.height);return SfntStatus::ok;
    }
    first=l.first_cluster;last=first+l.cluster_count;
    while(first<last){const u32 middle=first+(last-first)/2;const auto& item=h.clusters[middle];const u32 next=middle+1;
        // Pick the drawing group before resolving its final cluster. Its end
        // uses the ordered run sum, which can differ from left + cluster width.
        const float right=next==static_cast<u32>(l.first_cluster)+l.cluster_count?l.caret_end:
            (h.clusters[next].advance_flags&native_hit_group_start)?h.clusters[next].left:item.point_edge;
        if(right<=x)first=middle+1;else last=middle;}
    u32 c=minimum(first,static_cast<u32>(l.first_cluster)+l.cluster_count-1);
    // A point left of the line skips complete zero-width drawing groups,
    // while retaining a leading zero-width cluster within a nonempty group.
    if(x<0&&!h.clusters[c].width){const u32 limit=static_cast<u32>(l.first_cluster)+l.cluster_count;
        while(c<limit){u32 next=c+1;while(next<limit&&!(h.clusters[next].advance_flags&native_hit_group_start))++next;
            if(next==limit||h.clusters[next].left!=0)break;c=next;}}
    // CR/LF has one breaking cluster but two position metrics. A mouse beyond
    // the line hits its first control, never the boundary inside the newline.
    while(c>l.first_cluster&&(h.clusters[c].advance_flags&native_hit_newline)&&(h.clusters[c-1].advance_flags&native_hit_newline))--c;
    const auto& item=h.clusters[c];const auto m=metric(h,c,line);
    const float half=_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(item.width),_mm_set_ss(.5f)));
    const bool trailing=!(item.advance_flags&native_hit_newline)&&x>=layout_add(item.left,half);
    out->source_position=trailing?item.source_end:item.source_first;if(character)*character=item.source_first;
    out->inside=x>=0&&x<=l.caret_end&&y>=m.top&&y<=layout_add(m.top,m.height);
    return SfntStatus::ok;
}
SfntStatus native_paragraph_hit_range(const NativeParagraphHits& h,u32 first,u32 length,float ox,float oy,Slice<NativeParagraphHitMetric> output,u32* count) noexcept {
    if(!count)return SfntStatus::invalid_input;*count=0;
    if(!valid(h)||first>h.source_units||length>h.source_units-first||!layout_finite(ox)||!layout_finite(oy)||(output.count&&!output.data))return SfntStatus::invalid_input;
    const u32 begin=cluster_at(h,first),end=cluster_at(h,first+length);u32 c=begin,line=line_at(h,begin);
    if(h.visual_runs.count){
        do{const auto& l=h.lines[line];const u32 limit=minimum(end,static_cast<u32>(l.first_cluster)+l.cluster_count);
            if(c==limit){const auto m=metric(h,c,line);float left=m.left,width=0;const auto* run=run_for(h,c);
                if(run&&!run_part(h,*run,c,c,&left,&width))return SfntStatus::invalid_input;
                if(*count<output.count)output[*count]={m.source_first,0,layout_add(ox,left),layout_add(oy,l.top),0,line_height(h,l),m.bidi_level};++*count;
            }
            while(c<limit){const u32 source=h.clusters[c].source_first;const auto* initial=run_for(h,c);if(!initial)return SfntStatus::invalid_input;
                const u32 level=initial->bidi_level;float left=0,width=0;bool first_part=true;
                do{const auto* run=run_for(h,c);if(!run)return SfntStatus::invalid_input;if(run->bidi_level!=level)break;
                    const u32 next=minimum(limit,static_cast<u32>(run->first_cluster)+run->cluster_count);float part_left=0,part_width=0;
                    if(next<=c||!run_part(h,*run,c,next,&part_left,&part_width))return SfntStatus::invalid_input;
                    left=first_part?part_left:minimum(left,part_left);width=layout_add(width,part_width);first_part=false;c=next;
                }while(c<limit);
                const u32 source_end=c<h.clusters.count?h.clusters[c].source_first:h.source_units;
                if(*count<output.count)output[*count]={static_cast<u16>(source),static_cast<u16>(source_end-source),layout_add(ox,left),layout_add(oy,l.top),width,line_height(h,l),level};++*count;
            }
            ++line;
        }while(c<end&&line<h.lines.count);
        if(end>begin&&end<h.clusters.count&&first+length>h.clusters[end].source_first&&(h.clusters[end].advance_flags&native_hit_terminal_region)){
            const auto* run=run_for(h,end);const auto* before=run_for(h,end-1);const u32 tail_line=line_at(h,end);
            if(!run||!before)return SfntStatus::invalid_input;
            if(tail_line!=line_at(h,end-1)||run->bidi_level!=before->bidi_level){float left=0,width=0;
                if(!run_part(h,*run,end,end,&left,&width))return SfntStatus::invalid_input;
                const auto& l=h.lines[tail_line];
                if(*count<output.count)output[*count]={h.clusters[end].source_first,0,layout_add(ox,left),layout_add(oy,l.top),0,line_height(h,l),run->bidi_level};++*count;
            }
        }
        return *count>output.count?SfntStatus::output_too_small:SfntStatus::ok;
    }
    // A raw zero-length range still returns the snapped caret region.
    do{
        const auto& l=h.lines[line];if(l.first_cluster>h.clusters.count||l.cluster_count>h.clusters.count-l.first_cluster)return SfntStatus::invalid_input;
        const u32 limit=minimum(end,static_cast<u32>(l.first_cluster)+l.cluster_count);const auto start_metric=metric(h,c,line);float width=0,group=0;
        // A complete line uses the same ordered run reduction as caret_end.
        // Reuse it rather than visiting every glyph in off-screen selections.
        const bool whole=c==l.first_cluster&&limit==static_cast<u32>(l.first_cluster)+l.cluster_count;
        if(whole)width=l.caret_end;
        for(u32 i=c;!whole&&i<limit;++i){const auto& item=h.clusters[i];if(item.advance_flags&native_hit_group_start){width=layout_add(width,group);group=0;}
            ConstSlice<float> advances{};if(!native_paragraph_hit_advances(h,i,&advances))return SfntStatus::invalid_input;
            for(u32 g=0;g<advances.count;++g)group=layout_add(group,advances[g]);}
        width=layout_add(width,group);if(!layout_finite(width))return SfntStatus::resource_limit;
        const u32 source_end=limit<h.clusters.count?h.clusters[limit].source_first:h.source_units;
        const float left=layout_add(ox,start_metric.left),top=layout_add(oy,l.top);if(!layout_finite(left)||!layout_finite(top))return SfntStatus::resource_limit;
        if(*count<output.count)output[*count]={start_metric.source_first,static_cast<u16>(source_end-start_metric.source_first),left,top,width,line_height(h,l)};
        ++*count;c=limit;++line;
    }while(c<end&&line<h.lines.count);
    return *count>output.count?SfntStatus::output_too_small:SfntStatus::ok;
}
}
