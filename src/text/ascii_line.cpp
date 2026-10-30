#include "unicode_line_geometry.hpp"
#include "maicord/core/memory.hpp"
#include "layout_float.hpp"
namespace maicord {
SfntStatus native_ascii_line(const NativeRunPlanStore& store,NativeRunPlanHandle handle,
    Utf8View input,bool independent,float em,const NativeAsciiLineStorage& s,NativeAsciiLineResult* output,SfntShapeLimits limits,float trailing_spacing) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    NativeAsciiLineResult line{};const auto status=native_ascii_run(store,handle,input,independent,em,s.run,&line.run,limits);
    if(status!=SfntStatus::ok)return status;
    return native_finish_line(store,handle,input,em,s,line,output,limits,trailing_spacing,false,NativeLineText::printable_ascii);
}
SfntStatus native_ascii_line_position(ConstSlice<NativeLineCluster> clusters,const NativeAsciiLineResult& line,
    u32 position,bool trailing,NativeLinePosition* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
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
