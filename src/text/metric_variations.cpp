#include "maicord/text/metric_variations.hpp"
#include "maicord/text/base.hpp"
namespace maicord {namespace {
u16 r16(ConstSlice<u8> b,u32 at) noexcept {return byte_load_be16(b.data+at);}
u32 r32(ConstSlice<u8> b,u32 at) noexcept {return byte_load_be32(b.data+at);}
bool span(ConstSlice<u8> b,u32 at,u32 n) noexcept {return b.data && at<=b.count && n<=b.count-at;}
struct Report {u32 initial;const u32& left;u32* used;~Report() noexcept {if(used)*used=initial-left;}};
}
SfntStatus sfnt_hvar_open(ConstSlice<u8> table,SfntHvar* output,u32 work,u32* used) noexcept {
    Report report{work,work,used};if(!output)return SfntStatus::invalid_input;*output={};
    if(!span(table,0,20))return SfntStatus::truncated;
    if(r16(table,0)!=1 || r16(table,2)!=0)return SfntStatus::unsupported;
    const u32 store=r32(table,4),map=r32(table,8);
    if(store<20 || (map && map<20))return SfntStatus::malformed;
    SfntHvar result{};u32 child=0;auto s=item_variation_store_open(table,store,&result.store,work,&child);work-=child;if(s!=SfntStatus::ok)return s;
    if(map){if(!span(table,map,1))return SfntStatus::truncated;if(table[map]!=0)return SfntStatus::unsupported;
        s=variation_index_map_open(table,map,&result.advances,work,&child);work-=child;if(s!=SfntStatus::ok)return s;}
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_mvar_open(ConstSlice<u8> table,SfntMvar* output,u32 work,u32* used) noexcept {
    Report report{work,work,used};if(!output)return SfntStatus::invalid_input;*output={};
    if(!span(table,0,12))return SfntStatus::truncated;if(r16(table,0)!=1)return SfntStatus::unsupported;
    const u16 stride=r16(table,6),count=r16(table,8),store=r16(table,10);
    // Windows accepts an empty MVAR with zero stride. There are no records to
    // address in that case; generators commonly emit this compact empty form.
    if(!count)return store?SfntStatus::malformed:SfntStatus::ok;
    if(stride<8)return SfntStatus::malformed;const u32 size=static_cast<u32>(count)*stride;
    if(!span(table,12,size))return SfntStatus::truncated;
    if(store<12+size)return SfntStatus::malformed;
    SfntMvar result{};result.records={table.data+12,size};result.record_stride=stride;result.record_count=count;
    if(count>work)return SfntStatus::resource_limit;work-=count;
    for(u32 i=1;i<count;++i)if(r32(result.records,i*stride)<=r32(result.records,(i-1)*stride))return SfntStatus::malformed;
    u32 child=0;const auto s=item_variation_store_open(table,store,&result.store,work,&child);work-=child;if(s!=SfntStatus::ok)return s;
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_hvar_advance_delta(const SfntHvar& view,ConstSlice<double> scalars,u16 glyph,double* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output=0;u32 index=0;const auto s=variation_index_map_get(view.advances,glyph,&index);if(s!=SfntStatus::ok)return s;
    return item_variation_delta({view.store,scalars},index,output);
}
#pragma float_control(precise,on,push)
SfntStatus sfnt_hvar_design_advance(const SfntHvar& view,ConstSlice<double> scalars,const SfntHorizontalMetrics& metrics,u16 glyph,u16* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output=0;SfntGlyphMetrics base{};
    auto s=sfnt_horizontal_metrics(metrics,glyph,&base);if(s!=SfntStatus::ok)return s;
    if(!metrics.long_metrics)return SfntStatus::invalid_input;
    // The Windows design-metric path applies the last long metric's
    // HVAR entry to the compressed hmtx tail, including an explicit index map.
    double delta=0;s=sfnt_hvar_advance_delta(view,scalars,static_cast<u16>(minimum(static_cast<u32>(glyph),static_cast<u32>(metrics.long_metrics-1))),&delta);if(s!=SfntStatus::ok)return s;
    const double value=base.advance+delta+.5;
    if(!(value>=-9.e15 && value<=9.e15))return SfntStatus::malformed;
    const i64 integer=static_cast<i64>(value);*output=static_cast<u16>(integer-(static_cast<double>(integer)>value?1:0));return SfntStatus::ok;
}
#pragma float_control(pop)
SfntStatus sfnt_mvar_delta(const SfntMvar& view,ConstSlice<double> scalars,u32 tag,double* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output=0;
    u32 low=0,high=view.record_count;
    while(low<high){const u32 middle=low+(high-low)/2,value=r32(view.records,middle*view.record_stride);if(value<tag)low=middle+1;else high=middle;}
    if(low==view.record_count || r32(view.records,low*view.record_stride)!=tag)return SfntStatus::ok;
    return item_variation_delta({view.store,scalars},r32(view.records,low*view.record_stride+4),output);
}
} // namespace maicord
