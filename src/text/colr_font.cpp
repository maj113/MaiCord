#include "color_internal.hpp"

namespace maicord {
using namespace color_detail;
namespace {
SfntStatus records(ConstSlice<u8> bytes,u32 offset,u32 count,u32 stride,ConstSlice<u8>* output) noexcept {
    if(!count){*output={};return SfntStatus::ok;}
    if(!offset || offset>bytes.count || count>(bytes.count-offset)/stride)return SfntStatus::truncated;
    *output=view(bytes,offset,count*stride);return SfntStatus::ok;
}
u32 find_glyph(ConstSlice<u8> records,u32 stride,u16 glyph) noexcept {
    u32 low=0,high=records.count/stride;
    while(low<high) {const u32 mid=low+(high-low)/2;if(read16(records,mid*stride)<glyph)low=mid+1;else high=mid;}
    return low;
}
}

SfntStatus colr_open(const SfntFace& face,ColrFont* output) noexcept {
    if(!output)return SfntStatus::invalid_input;
    *output={};ColrFont result{};
    auto status=sfnt_table(face,sfnt_tag('C','O','L','R'),&result.table);
    if(status!=SfntStatus::ok)return status;
    status=cpal_open(face,&result.palette);if(status!=SfntStatus::ok)return status;
    status=sfnt_glyph_count(face,&result.glyph_count);if(status!=SfntStatus::ok)return status;
    const auto t=result.table;
    if(!range(t,0,14))return SfntStatus::truncated;
    result.version=read16(t,0);
    if(result.version>1)return SfntStatus::unsupported;
    if(result.version && !range(t,0,34))return SfntStatus::truncated;
    const u32 bases=read16(t,2),layers=read16(t,12);
    status=records(t,read32(t,4),bases,6,&result.bases_v0);if(status!=SfntStatus::ok)return status;
    status=records(t,read32(t,8),layers,4,&result.layers_v0);if(status!=SfntStatus::ok)return status;
    for(u32 i=0;i<bases;++i) {
        const u32 at=i*6,glyph=read16(result.bases_v0,at);
        if(glyph>=result.glyph_count || (i && glyph<=read16(result.bases_v0,at-6)))return SfntStatus::malformed;
    }
    if(result.version) {
        result.base_list=read32(t,14);result.layer_list=read32(t,18);result.clip_list=read32(t,22);
        result.variation_map=read32(t,26);result.variation_store=read32(t,30);
        if(result.base_list) {
            if(!range(t,result.base_list,4))return SfntStatus::truncated;
            const u32 count=read32(t,result.base_list);
            status=records(t,result.base_list+4,count,6,&result.bases_v1);if(status!=SfntStatus::ok)return status;
            for(u32 i=0;i<count;++i) {
                const u32 at=i*6,glyph=read16(result.bases_v1,at);
                if(glyph>=result.glyph_count || (i && glyph<=read16(result.bases_v1,at-6)))return SfntStatus::malformed;
            }
        }
        if(result.layer_list) {
            if(!range(t,result.layer_list,4))return SfntStatus::truncated;
            const u32 count=read32(t,result.layer_list);
            status=records(t,result.layer_list+4,count,4,&result.layers_v1);if(status!=SfntStatus::ok)return status;
        }
        if(result.clip_list) {
            if(!range(t,result.clip_list,5))return SfntStatus::truncated;
            if(t[result.clip_list]!=1)return SfntStatus::unsupported;
            const u32 count=read32(t,result.clip_list+1);
            status=records(t,result.clip_list+5,count,7,&result.clips);if(status!=SfntStatus::ok)return status;
            for(u32 i=0;i<count;++i) {
                const u32 at=i*7,first=read16(result.clips,at),last=read16(result.clips,at+2);
                if(first>last || last>=result.glyph_count || (i && first<=read16(result.clips,at-5)))return SfntStatus::malformed;
            }
        }
        // These locations are retained for the variation resolver. Merely
        // opening the color table does not validate or evaluate variation data.
        if((result.variation_map && !range(t,result.variation_map,4)) ||
           (result.variation_store && !range(t,result.variation_store,8)))return SfntStatus::truncated;
    }
    *output=result;return SfntStatus::ok;
}
SfntStatus colr_base_paint(const ColrFont& font,u16 glyph,u32* offset) noexcept {
    if(!offset)return SfntStatus::invalid_input;*offset=0;
    if(!font.table.data)return SfntStatus::invalid_input;
    if(glyph>=font.glyph_count)return SfntStatus::glyph_out_of_range;
    const u32 i=find_glyph(font.bases_v1,6,glyph);
    if(i==font.bases_v1.count/6 || read16(font.bases_v1,i*6)!=glyph)return SfntStatus::unsupported_sequence;
    return reference(font.table,font.base_list,read32(font.bases_v1,i*6+2),1,offset);
}
SfntStatus colr_legacy_layers(const ColrFont& font,u16 glyph,ColrLayerRange* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!font.table.data)return SfntStatus::invalid_input;
    if(glyph>=font.glyph_count)return SfntStatus::glyph_out_of_range;
    const u32 i=find_glyph(font.bases_v0,6,glyph);
    if(i==font.bases_v0.count/6 || read16(font.bases_v0,i*6)!=glyph)return SfntStatus::unsupported_sequence;
    const u32 first=read16(font.bases_v0,i*6+2),count=read16(font.bases_v0,i*6+4),layers=font.layers_v0.count/4;
    if(first>layers || count>layers-first)return SfntStatus::malformed;
    *output={first,count};return SfntStatus::ok;
}
SfntStatus colr_legacy_layer(const ColrFont& font,u32 index,ColrLegacyLayer* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!font.table.data || index>=font.layers_v0.count/4)return SfntStatus::invalid_input;
    const u16 glyph=read16(font.layers_v0,index*4),palette=read16(font.layers_v0,index*4+2);
    if(glyph>=font.glyph_count || !palette_valid(font,palette))return SfntStatus::malformed;
    *output={glyph,palette};return SfntStatus::ok;
}
SfntStatus colr_layer_paint(const ColrFont& font,u32 index,u32* offset) noexcept {
    if(!offset)return SfntStatus::invalid_input;*offset=0;
    if(!font.table.data || index>=font.layers_v1.count/4)return SfntStatus::invalid_input;
    return reference(font.table,font.layer_list,read32(font.layers_v1,index*4),1,offset);
}
SfntStatus colr_clip(const ColrFont& font,u16 glyph,ColrClip* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};output->variation_index=0xffffffff;
    if(!font.table.data)return SfntStatus::invalid_input;
    if(glyph>=font.glyph_count)return SfntStatus::glyph_out_of_range;
    u32 low=0,high=font.clips.count/7;
    while(low<high) {const u32 mid=low+(high-low)/2;if(read16(font.clips,mid*7+2)<glyph)low=mid+1;else high=mid;}
    if(low==font.clips.count/7 || glyph<read16(font.clips,low*7))return SfntStatus::ok;
    u32 at=0;auto status=reference(font.table,font.clip_list,read24(font.clips,low*7+4),1,&at);
    if(status!=SfntStatus::ok)return status;
    if(font.table[at]!=1 && font.table[at]!=2)return SfntStatus::unsupported;
    const bool variable=font.table[at]==2;
    if(!range(font.table,at,variable?13u:9u))return SfntStatus::truncated;
    *output={signed16(font.table,at+1),signed16(font.table,at+3),signed16(font.table,at+5),signed16(font.table,at+7),
             variable?read32(font.table,at+9):0xffffffffu,true,variable};return SfntStatus::ok;
}
} // namespace maicord
