#include "maicord/text/ascii_line.hpp"
#include <emmintrin.h>
namespace maicord {namespace {
float add(float a,float b) noexcept {return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a),_mm_set_ss(b)));}
bool finite(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);return (bits&0x7f800000u)!=0x7f800000u;}
}
SfntStatus native_ascii_line(const NativeRunPlanStore& store,NativeRunPlanHandle handle,
    Utf8View input,bool independent,float em,const NativeAsciiLineStorage& s,NativeAsciiLineResult* output,SfntShapeLimits limits,float trailing_spacing) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(s.clusters.count&&!s.clusters.data)return SfntStatus::invalid_input;
    NativeAsciiLineResult line{};auto status=native_ascii_run(store,handle,input,independent,em,s.run,&line.run,limits);
    if(status!=SfntStatus::ok)return status;
    u32 spacing_bits=0;copy_bytes(&spacing_bits,&trailing_spacing,4);
    if(spacing_bits&0x7fffffffu){status=sfnt_trailing_spacing({s.run.placements.data,line.run.glyph_count},
        line.run.nominal_ranges?ConstSlice<u8>{}:ConstSlice<u8>{s.run.shaping.flags.data,line.run.glyph_count},trailing_spacing,limits.stage_work);
        if(status!=SfntStatus::ok)return status;}
    const auto* metrics=native_run_plan_metrics(store,handle);if(!metrics)return SfntStatus::invalid_input;
    status=sfnt_single_font_line(*metrics,em,&line.vertical);if(status!=SfntStatus::ok)return status;
    if(input.bytes&&(!line.run.glyph_count||s.run.character_glyphs[0]))return SfntStatus::unsupported_sequence;
    float pen=0,last_nonspace_pen=0,tail_pen=0;line.geometry_flags=native_line_ordered_x;
    for(u32 at=0;at<input.bytes;){const u32 glyph=s.run.character_glyphs[at];u32 end=at+1;
        while(end<input.bytes&&s.run.character_glyphs[end]==glyph)++end;
        const u32 glyph_end=end<input.bytes?s.run.character_glyphs[end]:line.run.glyph_count;
        if(glyph_end<=glyph||glyph_end>line.run.glyph_count)return SfntStatus::unsupported_sequence;
        if(line.cluster_count==s.clusters.count)return SfntStatus::output_too_small;
        const bool space=input.data[at]==' ';const float left=pen;float width=0;
        for(u32 i=glyph;i<glyph_end;++i){const float advance=s.run.placements[i].advance;
            width=add(width,advance);pen=add(pen,advance);if(space)tail_pen=add(tail_pen,advance);}
        line.width_including_trailing=add(line.width_including_trailing,width);
        if(!finite(pen)||!finite(width)||!finite(line.width_including_trailing)||!finite(tail_pen))return SfntStatus::resource_limit;
        s.clusters[line.cluster_count++]={end,left,width};
        if(width<0||pen<left)line.geometry_flags&=~native_line_ordered_x;
        if(space)line.trailing_source_units+=end-at;
        else{line.width=line.width_including_trailing;line.trailing_source_units=0;last_nonspace_pen=pen;tail_pen=0;}
        at=end;
    }
    line.caret_end=add(last_nonspace_pen,tail_pen);if(!finite(line.caret_end))return SfntStatus::resource_limit;
    *output=line;return SfntStatus::ok;
}
SfntStatus native_ascii_line_position(ConstSlice<NativeLineCluster> clusters,const NativeAsciiLineResult& line,
    u32 position,bool trailing,NativeLinePosition* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(clusters.count!=line.cluster_count||(clusters.count&&!clusters.data)||position>line.run.source_units)return SfntStatus::invalid_input;
    if(position==line.run.source_units){*output={position,0,line.caret_end,line.caret_end,0};return SfntStatus::ok;}
    u32 first=0,last=clusters.count;while(first<last){const u32 middle=first+(last-first)/2;
        if(clusters[middle].source_end<=position)first=middle+1;else last=middle;}
    if(first==clusters.count)return SfntStatus::invalid_input;
    const auto& c=clusters[first];const u32 start=first?clusters[first-1].source_end:0;
    if(start>position||c.source_end>line.run.source_units)return SfntStatus::invalid_input;
    *output={start,c.source_end-start,trailing?add(c.left,c.width):c.left,c.left,c.width};return SfntStatus::ok;
}
SfntStatus native_ascii_line_point(ConstSlice<NativeLineCluster> clusters,const NativeAsciiLineResult& line,
    float x,float y,NativeLinePoint* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(clusters.count!=line.cluster_count||(clusters.count&&!clusters.data)||!finite(x)||!finite(y))return SfntStatus::invalid_input;
    if(!(line.geometry_flags&native_line_ordered_x))return SfntStatus::unsupported_sequence;
    if(!clusters.count)return SfntStatus::ok;
    u32 first=0,last=clusters.count;while(first<last){const u32 middle=first+(last-first)/2;
        if(clusters[middle].left<=x)first=middle+1;else last=middle;}
    const u32 index=first?first-1:0;const auto& cluster=clusters[index];const u32 start=index?clusters[index-1].source_end:0;
    const float half=_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(cluster.width),_mm_set_ss(.5f)));
    *output={x>=add(cluster.left,half)?cluster.source_end:start,
        x>=0&&x<=line.caret_end&&y>=0&&y<=line.vertical.height};return SfntStatus::ok;
}
SfntStatus native_ascii_line_range(ConstSlice<NativeLineCluster> clusters,const NativeAsciiLineResult& line,
    ConstSlice<u32> source_glyphs,ConstSlice<SfntPlacement> placements,u32 first,u32 length,
    NativeLineSelection* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(first>line.run.source_units||length>line.run.source_units-first||source_glyphs.count!=line.run.source_units||
       placements.count!=line.run.glyph_count||(source_glyphs.count&&!source_glyphs.data)||(placements.count&&!placements.data)||
       line.trailing_source_units>line.run.source_units||clusters.count!=line.cluster_count||(clusters.count&&!clusters.data))return SfntStatus::invalid_input;
    if(!length)return SfntStatus::ok;
    NativeLinePosition start{},end{};auto status=native_ascii_line_position(clusters,line,first,false,&start);if(status!=SfntStatus::ok)return status;
    status=native_ascii_line_position(clusters,line,first+length,false,&end);if(status!=SfntStatus::ok)return status;
    const u32 glyph_start=start.source_first<source_glyphs.count?source_glyphs[start.source_first]:placements.count;
    const u32 glyph_end=end.source_first<source_glyphs.count?source_glyphs[end.source_first]:placements.count;
    const u32 trim_source=line.run.source_units-line.trailing_source_units;
    const u32 trim_glyph=trim_source<source_glyphs.count?source_glyphs[trim_source]:placements.count;
    if(end.source_first<start.source_first||glyph_start>glyph_end||glyph_end>placements.count||trim_glyph>placements.count)return SfntStatus::invalid_input;
    if(glyph_end-glyph_start>work)return SfntStatus::resource_limit;
    const u32 split=minimum(glyph_end,maximum(glyph_start,trim_glyph));float width=0,tail=0;
    for(u32 i=glyph_start;i<split;++i)width=add(width,placements[i].advance);
    for(u32 i=split;i<glyph_end;++i)tail=add(tail,placements[i].advance);
    width=add(width,tail);if(!finite(width))return SfntStatus::resource_limit;
    *output={start.source_first,end.source_first-start.source_first,start.left,width,true};return SfntStatus::ok;
}
}
