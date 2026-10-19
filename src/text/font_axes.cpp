#include "maicord/text/font_axes.hpp"
#include "maicord/text/base.hpp"

namespace maicord {
namespace {
bool span(ConstSlice<u8> b,u32 at,u32 size) noexcept {return b.data && at<=b.count && size<=b.count-at;}
u16 r16(const u8* p) noexcept {return byte_load_be16(p);}
i32 fixed(const u8* p) noexcept {return static_cast<i32>(byte_load_be32(p));}
bool spend(u32* work) noexcept {if(!*work)return false;--*work;return true;}
struct Report {u32 initial;const u32& left;u32* used;~Report() noexcept {if(used)*used=initial-left;}};
}
SfntStatus font_axis(const FontAxes& axes,u32 index,FontAxis* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(index>=axes.axis_count)return SfntStatus::invalid_input;
    const auto* p=axes.axes.data+index*axes.axis_stride;
    *output={byte_load_be32(p),fixed(p+4),fixed(p+8),fixed(p+12),r16(p+16),r16(p+18)};return SfntStatus::ok;
}
SfntStatus font_named_instance(const FontAxes& axes,u32 index,FontNamedInstance* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(index>=axes.instance_count)return SfntStatus::invalid_input;
    const auto* p=axes.instances.data+index*axes.instance_stride;
    const u32 size=axes.axis_count*4u;
    *output={{p+4,size},r16(p),axes.instance_stride>=size+6?r16(p+4+size):static_cast<u16>(0xffff)};
    return SfntStatus::ok;
}
SfntStatus font_axes_table_open(ConstSlice<u8> table,FontAxes* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!span(table,0,16))return SfntStatus::truncated;
    if(r16(table.data)!=1)return SfntStatus::unsupported;
    const u32 first=r16(table.data+4),count=r16(table.data+8),stride=r16(table.data+10);
    // A zero-axis fvar is non-variable, regardless of unused variation data.
    if(!count)return SfntStatus::ok;
    const u32 instances=r16(table.data+12),instance_stride=r16(table.data+14),coordinates=count*4;
    if(first<16 || stride<20 || instance_stride<coordinates+4)return SfntStatus::malformed;
    if(!span(table,first,count*stride))return SfntStatus::truncated;
    const u32 instance_at=first+count*stride;
    if(!span(table,instance_at,instances*instance_stride))return SfntStatus::truncated;
    FontAxes result{{table.data+first,count*stride},{table.data+instance_at,instances*instance_stride},
        static_cast<u16>(count),static_cast<u16>(stride),static_cast<u16>(instances),static_cast<u16>(instance_stride)};
    for(u32 i=0;i<count;++i) {
        if(!spend(&work))return SfntStatus::resource_limit;
        FontAxis a{};font_axis(result,i,&a);
        if(a.minimum>a.normal || a.normal>a.maximum)return SfntStatus::malformed;
        for(u32 j=0;j<i;++j) {
            if(!spend(&work))return SfntStatus::resource_limit;
            if(byte_load_be32(result.axes.data+j*stride)==a.tag)return SfntStatus::malformed;
        }
    }
    // Named coordinates are clamped at selection, just as arbitrary settings.
    // Record sizes (including compatible extension bytes) determine stride.
    *output=result;return SfntStatus::ok;
}
SfntStatus font_axes_open(const SfntFace& face,FontAxes* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    ConstSlice<u8> table{};const auto status=sfnt_table(face,sfnt_tag('f','v','a','r'),&table);
    if(status==SfntStatus::missing_table)return SfntStatus::ok;
    return status==SfntStatus::ok?font_axes_table_open(table,output,work):status;
}
SfntStatus font_axis_mappings_table_open(ConstSlice<u8> table,u16 count,Slice<AxisSegmentMap> storage,
                                        FontAxisMappings* output,u32 work,u32* used) noexcept {
    Report report{work,work,used};
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!count)return SfntStatus::ok;
    if(!span(table,0,8))return SfntStatus::truncated;
    if(r16(table.data)!=1)return SfntStatus::unsupported;
    if(r16(table.data+6)!=count)return SfntStatus::malformed;
    if(storage.count<count || !storage.data)return SfntStatus::output_too_small;
    u32 at=8;
    for(u32 axis=0;axis<count;++axis) {
        if(!spend(&work))return SfntStatus::resource_limit;
        if(!span(table,at,2))return SfntStatus::truncated;
        const u32 pairs=r16(table.data+at);at+=2;
        if(!span(table,at,pairs*4))return SfntStatus::truncated;
        const ConstSlice<u8> records{table.data+at,pairs*4};u32 anchors=0;
        i32 previous_from=-16385,previous_to=-16385;
        for(u32 i=0;i<pairs;++i) {
            if(!spend(&work))return SfntStatus::resource_limit;
            const i32 from=static_cast<i16>(r16(records.data+i*4)),to=static_cast<i16>(r16(records.data+i*4+2));
            if(from < -16384 || from > 16384 || to < -16384 || to > 16384 || from<=previous_from || to<previous_to)return SfntStatus::malformed;
            if(from==to){if(from==-16384)anchors|=1;else if(!from)anchors|=2;else if(from==16384)anchors|=4;}
            previous_from=from;previous_to=to;
        }
        storage[axis]={records,anchors==7};at+=pairs*4;
    }
    *output={{storage.data,count}};return SfntStatus::ok;
}
SfntStatus font_axis_mappings_open(const SfntFace& face,u16 count,Slice<AxisSegmentMap> storage,
                                  FontAxisMappings* output,u32 work,u32* used) noexcept {
    if(used)*used=0;
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!count)return SfntStatus::ok;
    ConstSlice<u8> table{};const auto status=sfnt_table(face,sfnt_tag('a','v','a','r'),&table);
    if(status==SfntStatus::missing_table)return SfntStatus::ok;
    return status==SfntStatus::ok?font_axis_mappings_table_open(table,count,storage,output,work,used):status;
}

#pragma float_control(precise,on,push)
namespace {
double rounded(double value) noexcept {
    const double shifted=value+.5;const i64 integer=static_cast<i64>(shifted);
    return static_cast<double>(integer)-(static_cast<double>(integer)>shifted?1:0);
}
double quantized(double value) noexcept {return rounded(value*65536.0)/65536.0;}
double remap(const AxisSegmentMap& map,double value) noexcept {
    if(!map.enabled)return value;
    // Validated sorted maps allow logarithmic lookup without a copied index.
    u32 first=0,last=map.pairs.count/4;
    while(first<last){const u32 middle=first+(last-first)/2;const double from=static_cast<i16>(r16(map.pairs.data+middle*4))/16384.0;
        if(from<value)first=middle+1;else last=middle;}
    const auto* end=map.pairs.data+first*4;
    const double end_from=static_cast<i16>(r16(end))/16384.0,end_to=static_cast<i16>(r16(end+2))/16384.0;
    if(value==end_from)return end_to;
    const double start_from=static_cast<i16>(r16(end-4))/16384.0,start_to=static_cast<i16>(r16(end-2))/16384.0;
    const double ratio=quantized((value-start_from)/(end_from-start_from));
    return start_to+quantized(ratio*(end_to-start_to));
}
}
SfntStatus font_axis_coordinates(const FontAxes& axes,const FontAxisMappings& mappings,
                                 ConstSlice<FontAxisSetting> settings,Slice<double> normalized,Slice<i16> conditions,
                                 FontAxisPrecision precision,u32 work,u32* used) noexcept {
    Report report{work,work,used};
    if((settings.count && !settings.data) || (mappings.axes.count && (mappings.axes.count!=axes.axis_count || !mappings.axes.data)))return SfntStatus::invalid_input;
    if(normalized.count<axes.axis_count || (axes.axis_count && !normalized.data) ||
       (conditions.count && (conditions.count<axes.axis_count || !conditions.data)))return SfntStatus::output_too_small;
    for(u32 i=0;i<settings.count;++i) {
        if(!spend(&work))return SfntStatus::resource_limit;
        const double value=settings[i].value;
        if(!(value>=-1.7976931348623157e308 && value<=1.7976931348623157e308))return SfntStatus::invalid_input;
    }
    for(u32 i=0;i<axes.axis_count;++i) {
        if(!spend(&work))return SfntStatus::resource_limit;
        FontAxis a{};font_axis(axes,i,&a);double user=a.normal/65536.0;
        for(u32 j=0;j<settings.count;++j){if(!spend(&work))return SfntStatus::resource_limit;if(settings[j].tag==a.tag)user=settings[j].value;}
        const double low=a.minimum/65536.0,normal=a.normal/65536.0,high=a.maximum/65536.0;
        if(user<low)user=low;if(user>high)user=high;
        if(precision==FontAxisPrecision::fixed16)user=quantized(user);
        double value=user<normal?(user-normal)/(normal-low):user>normal?(user-normal)/(high-normal):0;
        // Normalized coordinates are quantized before avar mapping.
        value=quantized(value);
        if(mappings.axes.count)value=remap(mappings.axes[i],value);
        value=quantized(value);if(value < -1)value=-1;if(value>1)value=1;
        normalized[i]=value;
        if(conditions.count)conditions[i]=static_cast<i16>(rounded(value*16384.0));
    }
    return SfntStatus::ok;
}
#pragma float_control(pop)
} // namespace maicord
