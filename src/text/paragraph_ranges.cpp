#include "maicord/text/ascii_paragraph.hpp"
#include "layout_float.hpp"
namespace maicord {namespace {
u32 cluster_at(const NativeParagraphGeometry& g,u32 source) noexcept {
    u32 lo=0,hi=g.clusters.count;while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(g.clusters[mid].source_end<=source)lo=mid+1;else hi=mid;}return lo;
}
u32 source_at(const NativeParagraphGeometry& g,u32 cluster) noexcept {return cluster<g.clusters.count?g.clusters[cluster].source_first:g.source_units;}
float glyph_width(const NativeParagraphGeometry& g,u32 first,u32 end) noexcept {
    float width=0;for(u32 i=first;i<end;++i)width=layout_add(width,g.placements[i].advance);return width;
}
float tab_next(float pen,float stop) noexcept {return static_cast<float>(static_cast<u32>(pen/stop)+1)*stop;}
float tab_pen(const NativeParagraphGeometry& g,const NativeParagraphLine& line,u32 end) noexcept {
    float pen=0;const u32 first=cluster_at(g,g.runs[line.first_run].source_first);
    // TAB paragraphs never use nominal metric groups. Their stop position is
    // derived from cluster metrics, independently of drawing-run rounding.
    for(u32 c=first;c<end;++c){const auto& item=g.clusters[c];pen=(item.flags&native_paragraph_tab)?tab_next(pen,g.tab_stop):layout_add(pen,item.width);}return pen;
}
void run_part(const NativeParagraphGeometry& g,const NativeParagraphLine& line,const NativeParagraphRun& run,u32 first,u32 end,float* left,float* width) noexcept {
    *left=run.x;*width=run.width;const u32 run_end=static_cast<u32>(run.source_first)+run.source_count;
    if(first<=run.source_first&&end>=run_end)return;
    const u32 begin=cluster_at(g,maximum(first,static_cast<u32>(run.source_first))),last=cluster_at(g,minimum(end,run_end));
    const u32 start=cluster_at(g,run.source_first);
    if(g.clusters[start].flags&native_paragraph_tab){float pen=tab_pen(g,line,start),prefix=0;*width=0;
        for(u32 c=start;c<last;++c){const float next=tab_next(pen,g.tab_stop),advance=next-pen;
            if(c<begin)prefix=layout_add(prefix,advance);else *width=layout_add(*width,advance);pen=next;}*left=layout_add(run.x,prefix);return;}
    const u32 first_glyph=begin<g.clusters.count?g.clusters[begin].glyph_first:g.placements.count;
    const u32 last_glyph=last<g.clusters.count?g.clusters[last].glyph_first:g.placements.count;
    *left=layout_add(run.x,glyph_width(g,run.glyph_first,first_glyph));*width=glyph_width(g,first_glyph,last_glyph);
}
}
SfntStatus native_paragraph_geometry_visit(const NativeParagraphGeometry& g,u32 first,u32 length,NativeParagraphRangeSink sink,u32* count,bool terminal) noexcept {
    if(!count)return SfntStatus::invalid_input;*count=0;
    if(!g.lines.count||!g.lines.data||!g.runs.count||!g.runs.data||!g.clusters.count||!g.clusters.data||(g.placements.count&&!g.placements.data)||
       g.source_units>65535||first>g.source_units||length>g.source_units-first)return SfntStatus::invalid_input;
    const u32 begin=source_at(g,cluster_at(g,first)),end=source_at(g,cluster_at(g,first+length));
    u32 lo=0,hi=g.runs.count;while(lo<hi){const u32 mid=lo+(hi-lo)/2;const auto& r=g.runs[mid];if(static_cast<u32>(r.source_first)+r.source_count<=begin)lo=mid+1;else hi=mid;}
    u32 run=lo;lo=0;hi=g.lines.count;while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(g.lines[mid].first_run<=run)lo=mid+1;else hi=mid;}u32 line=lo?lo-1:0;bool accepted=true;
    do{const auto& l=g.lines[line];const u32 limit=static_cast<u32>(l.first_run)+l.run_count;float left=l.extent,width=0;
        const u32 source_first=maximum(begin,l.run_count?static_cast<u32>(g.runs[l.first_run].source_first):g.source_units);u32 source_end=source_first;
        if(run<limit){left=g.runs[run].x;
            const bool whole=run==l.first_run&&begin<=g.runs[run].source_first&&end>=static_cast<u32>(g.runs[limit-1].source_first)+g.runs[limit-1].source_count;
            // A trimmed line can retain only a mandatory control positioned
            // after the omission sign. Its endpoint is not its range width.
            if(whole){width=layout_add(l.extent,-left);source_end=static_cast<u32>(g.runs[limit-1].source_first)+g.runs[limit-1].source_count;run=limit;}
            else{const u32 first_run=run;
            if(begin==end){float unused=0;run_part(g,l,g.runs[run],begin,end,&left,&unused);}
            while(run<limit&&g.runs[run].source_first<end){const auto& r=g.runs[run];float run_left=0,part=0;run_part(g,l,r,begin,end,&run_left,&part);if(run==first_run)left=run_left;width=layout_add(width,part);
                source_end=minimum(end,static_cast<u32>(r.source_first)+r.source_count);++run;}
            }
        }
        if(terminal&&run<limit&&g.runs[run].source_first==end)terminal=false;
        const NativeParagraphHitMetric metric{static_cast<u16>(source_first),static_cast<u16>(source_end-source_first),left,l.top,width,l.height};
        if(accepted&&sink.emit)accepted=sink.emit(sink.context,metric);++*count;++line;
    }while(run<g.runs.count&&(g.runs[run].source_first<end||(terminal&&g.runs[run].source_first==end))&&line<g.lines.count);
    return accepted?SfntStatus::ok:SfntStatus::output_too_small;
}
}
