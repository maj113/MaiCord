#include "color_internal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
using namespace color_detail;
namespace {
SfntStatus records(ConstSlice<u8> bytes,u32 offset,u32 count,u32 stride,ConstSlice<u8>* output) noexcept {
    if(!count){reset_value(output);return SfntStatus::ok;}
    if(!offset || offset>bytes.count || count>(bytes.count-offset)/stride)return SfntStatus::truncated;
    *output=view(bytes,offset,count*stride);return SfntStatus::ok;
}
// A 32-bit count at an offset, followed by that many records.
SfntStatus counted(ConstSlice<u8> bytes,u32 at,u32 stride,ConstSlice<u8>* output) noexcept {
    if(!span(bytes,at,4))return SfntStatus::truncated;
    return records(bytes,at+4,r32(bytes,at),stride,output);
}
// Base glyph records are six bytes each, strictly ascending by in-range glyph.
bool ordered(ConstSlice<u8> bases,u32 glyphs) noexcept {
    for(u32 i=0;i<bases.count/6;++i) {
        const u32 glyph=r16(bases,i*6);
        if(glyph>=glyphs || (i && glyph<=r16(bases,i*6-6)))return false;
    }
    return true;
}
u32 find_glyph(ConstSlice<u8> records,u32 stride,u16 glyph) noexcept {
    u32 low=0,high=records.count/stride;
    while(low<high) {const u32 mid=low+(high-low)/2;if(r16(records,mid*stride)<glyph)low=mid+1;else high=mid;}
    return low;
}
}
SfntStatus colr_open(const SfntFace& face,ColrFont* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);ColrFont& result=*output;
    const auto opened=[&]() noexcept { // Fills the zeroed output; cleared again on failure below.
    auto status=sfnt_table(face,sfnt_tag('C','O','L','R'),&result.table);
    if(status!=SfntStatus::ok)return status;
    status=cpal_open(face,&result.palette);if(status!=SfntStatus::ok)return status;
    status=sfnt_glyph_count(face,&result.glyph_count);if(status!=SfntStatus::ok)return status;
    const auto t=result.table;
    if(!span(t,0,14))return SfntStatus::truncated;
    result.version=r16(t,0);
    if(result.version>1)return SfntStatus::unsupported;
    if(result.version && !span(t,0,34))return SfntStatus::truncated;
    const u32 bases=r16(t,2),layers=r16(t,12);
    status=records(t,r32(t,4),bases,6,&result.bases_v0);if(status!=SfntStatus::ok)return status;
    status=records(t,r32(t,8),layers,4,&result.layers_v0);if(status!=SfntStatus::ok)return status;
    if(!ordered(result.bases_v0,result.glyph_count))return SfntStatus::malformed;
    if(result.version) {
        result.base_list=r32(t,14);result.layer_list=r32(t,18);result.clip_list=r32(t,22);
        result.variation_map=r32(t,26);result.variation_store=r32(t,30);
        if(result.base_list) {
            status=counted(t,result.base_list,6,&result.bases_v1);if(status!=SfntStatus::ok)return status;
            if(!ordered(result.bases_v1,result.glyph_count))return SfntStatus::malformed;
        }
        if(result.layer_list) {
            status=counted(t,result.layer_list,4,&result.layers_v1);if(status!=SfntStatus::ok)return status;
        }
        if(result.clip_list) {
            if(!span(t,result.clip_list,5))return SfntStatus::truncated;
            if(t[result.clip_list]!=1)return SfntStatus::unsupported;
            const u32 count=r32(t,result.clip_list+1);
            status=records(t,result.clip_list+5,count,7,&result.clips);if(status!=SfntStatus::ok)return status;
            for(u32 i=0;i<count;++i) {
                const u32 at=i*7,first=r16(result.clips,at),last=r16(result.clips,at+2);
                if(first>last || last>=result.glyph_count || (i && first<=r16(result.clips,at-5)))return SfntStatus::malformed;
            }
        }
        // Only the locations are checked and kept for the variation resolver;
        // variation data is neither validated nor evaluated at open.
        if((result.variation_map && !span(t,result.variation_map,4)) ||
           (result.variation_store && !span(t,result.variation_store,8)))return SfntStatus::truncated;
    }
    return SfntStatus::ok;
    }();
    if(opened!=SfntStatus::ok)reset_value(output);
    return opened;
}
SfntStatus colr_base_paint(const ColrFont& font,u16 glyph,u32* offset) noexcept {
    if(!offset)return SfntStatus::invalid_input;*offset=0;
    if(!font.table.data)return SfntStatus::invalid_input;
    if(glyph>=font.glyph_count)return SfntStatus::glyph_out_of_range;
    const u32 i=find_glyph(font.bases_v1,6,glyph);
    if(i==font.bases_v1.count/6 || r16(font.bases_v1,i*6)!=glyph)return SfntStatus::unsupported_sequence;
    return reference(font.table,font.base_list,r32(font.bases_v1,i*6+2),1,offset);
}
SfntStatus colr_legacy_layers(const ColrFont& font,u16 glyph,ColrLayerRange* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!font.table.data)return SfntStatus::invalid_input;
    if(glyph>=font.glyph_count)return SfntStatus::glyph_out_of_range;
    const u32 i=find_glyph(font.bases_v0,6,glyph);
    if(i==font.bases_v0.count/6 || r16(font.bases_v0,i*6)!=glyph)return SfntStatus::unsupported_sequence;
    const u32 first=r16(font.bases_v0,i*6+2),count=r16(font.bases_v0,i*6+4),layers=font.layers_v0.count/4;
    if(first>layers || count>layers-first)return SfntStatus::malformed;
    *output={first,count};return SfntStatus::ok;
}
SfntStatus colr_legacy_layer(const ColrFont& font,u32 index,ColrLegacyLayer* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!font.table.data || index>=font.layers_v0.count/4)return SfntStatus::invalid_input;
    const u16 glyph=r16(font.layers_v0,index*4),palette=r16(font.layers_v0,index*4+2);
    if(glyph>=font.glyph_count || !palette_valid(font,palette))return SfntStatus::malformed;
    *output={glyph,palette};return SfntStatus::ok;
}
SfntStatus colr_layer_paint(const ColrFont& font,u32 index,u32* offset) noexcept {
    if(!offset)return SfntStatus::invalid_input;*offset=0;
    if(!font.table.data || index>=font.layers_v1.count/4)return SfntStatus::invalid_input;
    return reference(font.table,font.layer_list,r32(font.layers_v1,index*4),1,offset);
}
SfntStatus colr_clip(const ColrFont& font,u16 glyph,ColrClip* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);output->variation_index=0xffffffff;
    if(!font.table.data)return SfntStatus::invalid_input;
    if(glyph>=font.glyph_count)return SfntStatus::glyph_out_of_range;
    u32 low=0,high=font.clips.count/7;
    while(low<high) {const u32 mid=low+(high-low)/2;if(r16(font.clips,mid*7+2)<glyph)low=mid+1;else high=mid;}
    if(low==font.clips.count/7 || glyph<r16(font.clips,low*7))return SfntStatus::ok;
    u32 at=0;auto status=reference(font.table,font.clip_list,r24(font.clips,low*7+4),1,&at);
    if(status!=SfntStatus::ok)return status;
    if(font.table[at]!=1 && font.table[at]!=2)return SfntStatus::unsupported;
    const bool variable=font.table[at]==2;
    if(!span(font.table,at,variable?13u:9u))return SfntStatus::truncated;
    *output={signed16(font.table,at+1),signed16(font.table,at+3),signed16(font.table,at+5),signed16(font.table,at+7),
             variable?r32(font.table,at+9):0xffffffffu,true,variable};return SfntStatus::ok;
}
} // namespace maicord
