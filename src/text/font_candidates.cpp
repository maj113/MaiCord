#include "maicord/text/font_candidates.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/font_names.hpp"
#include "sfnt_read.hpp"
namespace maicord {namespace {
using namespace sfnt_read;
constexpr u32 default_named=0xffffffffu;
i32 coordinate(const FontAxes& axes,u32 instance,u32 index,const FontAxis& axis) noexcept {
    i32 value=axis.normal;
    if(instance!=default_named)value=static_cast<i32>(byte_load_be32(axes.instances.data+instance*axes.instance_stride+4+index*4));
    return maximum(axis.minimum,minimum(axis.maximum,value));
}
SfntStatus properties(const NativeFontCandidates& source,u32 instance,NativeFontProperties* output) noexcept {
    auto result=source.base;
    for(u32 i=0;i<source.axes.axis_count;++i){FontAxis axis{};font_axis(source.axes,i,&axis);const i32 value=coordinate(source.axes,instance,i,axis);
        if(axis.tag==sfnt_tag('w','g','h','t')){if(value<65536||value>1000*65536||(value&65535))return SfntStatus::unsupported;result.weight=static_cast<u16>(value/65536);}
        else if(axis.tag==sfnt_tag('w','d','t','h')){
            const i32 widths[]{50*65536,125*32768,75*65536,175*32768,100*65536,225*32768,125*65536,150*65536,200*65536};
            u32 width=0;while(width<9&&widths[width]!=value)++width;if(width==9)return SfntStatus::unsupported;result.width=static_cast<u8>(width+1);
        }else return SfntStatus::unsupported;
    }*output=result;return SfntStatus::ok;
}
SfntStatus stat_axes(const SfntFace& face,const FontAxes& axes) noexcept {
    ConstSlice<u8> table{};auto status=sfnt_table(face,sfnt_tag('S','T','A','T'),&table);if(status==SfntStatus::missing_table)return SfntStatus::unsupported;if(status!=SfntStatus::ok)return status;
    if(table.count<18)return SfntStatus::truncated;const auto* p=table.data;const u16 minor=byte_load_be16(p+2);
    if(byte_load_be16(p)!=1||minor>2)return SfntStatus::unsupported;if(minor&&table.count<20)return SfntStatus::truncated;
    const u32 stride=byte_load_be16(p+4),count=byte_load_be16(p+6),offset=byte_load_be32(p+8);
    if(stride<8||count!=axes.axis_count)return SfntStatus::unsupported;
    if(offset<(minor?20u:18u))return SfntStatus::malformed;
    if(offset>table.count||static_cast<u64>(count)*stride>table.count-offset)return SfntStatus::truncated;
    for(u32 i=0;i<axes.axis_count;++i){FontAxis a{};font_axis(axes,i,&a);u32 matches=0;
        for(u32 j=0;j<count;++j){const auto* record=p+offset+j*stride;if(byte_load_be32(record)==a.tag&&byte_load_be16(record+4)==a.name)++matches;}
        if(matches!=1)return SfntStatus::unsupported;
    }return SfntStatus::ok;
}
}
SfntStatus native_font_candidates_open(const SfntFace& face,NativeFontCandidates* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!spend(work,1))return SfntStatus::resource_limit;
    SfntFontAttributes attributes{};auto status=sfnt_font_attributes(face,&attributes);if(status!=SfntStatus::ok)return status;
    NativeFontCandidates result{};result.base={attributes.weight,static_cast<u8>(attributes.width),static_cast<u8>(attributes.selection&512?1:attributes.selection&1?2:0)};
    status=font_axes_open(face,&result.axes,work);if(status!=SfntStatus::ok)return status;
    if(!result.axes.axis_count){result.count=1;*output=result;return SfntStatus::ok;}
    // The required variable profile has only weight and width axes.
    if(result.axes.axis_count>2)return SfntStatus::unsupported;
    if(!spend(work,result.axes.axis_count*(result.axes.axis_count+1)/2+result.axes.axis_count*result.axes.axis_count))return SfntStatus::resource_limit;
    status=stat_axes(face,result.axes);if(status!=SfntStatus::ok)return status;
    NativeFontProperties base{};status=properties(result,default_named,&base);if(status!=SfntStatus::ok)return status;
    ConstSlice<u8> names_table{};status=sfnt_table(face,sfnt_tag('n','a','m','e'),&names_table);if(status!=SfntStatus::ok)return status;
    SfntNames names{};status=sfnt_names_open(names_table,&names);if(status!=SfntStatus::ok)return status;
    if(!spend(work,names.records+names.language_tags))return SfntStatus::resource_limit;
    bool has_default=false;
    for(u32 i=0;i<result.axes.instance_count;++i){if(!spend(work,result.axes.axis_count*2+names.records+1))return SfntStatus::resource_limit;
        NativeFontProperties p{};status=properties(result,i,&p);if(status!=SfntStatus::ok)return status;
        FontNamedInstance instance{};font_named_instance(result.axes,i,&instance);SfntName name{};bool found=false;
        status=sfnt_name_english(names,instance.name,&name,&found);if(status!=SfntStatus::ok)return status;if(!found||!name.bytes.count)return SfntStatus::unsupported;
        if(!spend(work,name.bytes.count/2))return SfntStatus::resource_limit;u32 units=0;
        status=sfnt_name_decode(name,{},&units);if(status!=SfntStatus::output_too_small)return status==SfntStatus::ok?SfntStatus::unsupported:status;
        bool is_default=true;for(u32 j=0;j<result.axes.axis_count;++j){FontAxis axis{};font_axis(result.axes,j,&axis);is_default=is_default&&coordinate(result.axes,i,j,axis)==axis.normal;}
        has_default=has_default||is_default;
    }
    result.count=static_cast<u32>(result.axes.instance_count)+(has_default?0u:1u);*output=result;return SfntStatus::ok;
}
SfntStatus native_font_candidate(const NativeFontCandidates& source,u32 index,NativeFontCandidate* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!source.count||index>=source.count)return SfntStatus::invalid_input;
    const u32 named=index<source.axes.instance_count?index:default_named;NativeFontProperties p{};const auto status=properties(source,named,&p);if(status!=SfntStatus::ok)return status;
    *output={p,named};return SfntStatus::ok;
}
SfntStatus native_font_candidate_settings(const NativeFontCandidates& source,u32 index,Slice<FontAxisSetting> output,u32* count) noexcept {
    if(!count)return SfntStatus::invalid_input;*count=0;if(!source.count||index>=source.count||(output.count&&!output.data))return SfntStatus::invalid_input;
    if(output.count<source.axes.axis_count)return SfntStatus::output_too_small;
    const u32 named=index<source.axes.instance_count?index:default_named;
    for(u32 i=0;i<source.axes.axis_count;++i){FontAxis axis{};font_axis(source.axes,i,&axis);output[i]={axis.tag,coordinate(source.axes,named,i,axis)/65536.0};}
    *count=source.axes.axis_count;return SfntStatus::ok;
}
}
