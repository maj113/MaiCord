#include "maicord/text/ascii_line.hpp"
#include "layout_float.hpp"
namespace maicord {
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
            width=layout_add(width,advance);pen=layout_add(pen,advance);if(space)tail_pen=layout_add(tail_pen,advance);}
        line.width_including_trailing=layout_add(line.width_including_trailing,width);
        if(!is_finite(pen)||!is_finite(width)||!is_finite(line.width_including_trailing)||!is_finite(tail_pen))return SfntStatus::resource_limit;
        s.clusters[line.cluster_count++]={end,left,width};
        if(width<0||pen<left)line.geometry_flags&=~native_line_ordered_x;
        if(space)line.trailing_source_units+=end-at;
        else{line.width=line.width_including_trailing;line.trailing_source_units=0;last_nonspace_pen=pen;tail_pen=0;}
        at=end;
    }
    line.caret_end=layout_add(last_nonspace_pen,tail_pen);if(!is_finite(line.caret_end))return SfntStatus::resource_limit;
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
    *output={start,c.source_end-start,trailing?layout_add(c.left,c.width):c.left,c.left,c.width};return SfntStatus::ok;
}
}
