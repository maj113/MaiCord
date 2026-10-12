#include "maicord/text/gpos_execute.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {
SfntStatus sfnt_position_flags_from_clusters(const SfntClusterMap& clusters,SfntGlyphView glyphs,Slice<u8> output,ConstSlice<u8> zero,u64 work) noexcept {
    if(!sfnt_glyph_view_valid(glyphs) || glyphs.record_size<sizeof(SfntClusterGlyph) || (output.count && !output.data) || (zero.count && !zero.data))return SfntStatus::invalid_input;
    const u32 count=sfnt_glyph_count(glyphs);if(output.count<count || (zero.count && zero.count<count))return SfntStatus::output_too_small;
    if(zero.count && zero.data!=output.data){const usize a=reinterpret_cast<usize>(zero.data),b=reinterpret_cast<usize>(output.data);if(a>~usize{0}-count || b>~usize{0}-count || (a<b+count && b<a+count))return SfntStatus::invalid_input;}
    // Reserve the maximum query cost before output; repeated membership in
    // the same source interval reuses the previous boundary query.
    if(static_cast<u64>(count)*(static_cast<u64>(clusters.levels)*2+4)>work)return SfntStatus::resource_limit;
    u32 first=0xffffffffu,end=0,previous=0xffffffffu;
    for(u32 i=0;i<count;++i){const auto& glyph=sfnt_glyph_at(glyphs,i);const u32 source=byte_load_le32(reinterpret_cast<const u8*>(&glyph)+4);
        if(source>=clusters.text_count || static_cast<u8>(glyph.glyph_class)>4 || (zero.count && zero.data[i]>1))return SfntStatus::invalid_input;
        if(source<first || source>=end){const auto status=sfnt_cluster_map_find(clusters,source,&first,&end);if(status!=SfntStatus::ok)return status;}
        const bool zero_advance=zero.count?zero.data[i]!=0:glyph.glyph_class==SfntGlyphClass::mark;
        output.data[i]=static_cast<u8>((first!=previous?sfnt_position_cluster_start:0)|(zero_advance?sfnt_position_zero_advance:0));previous=first;
    }return SfntStatus::ok;
}
}
