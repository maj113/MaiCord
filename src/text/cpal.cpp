#include "color_internal.hpp"

namespace maicord {
using namespace color_detail;

SfntStatus cpal_open(const SfntFace& face,CpalFont* output) noexcept {
    if(!output)return SfntStatus::invalid_input;
    *output={}; ConstSlice<u8> bytes{};
    auto status=sfnt_table(face,sfnt_tag('C','P','A','L'),&bytes);
    if(status!=SfntStatus::ok)return status;
    if(!range(bytes,0,12))return SfntStatus::truncated;
    CpalFont result{};
    result.version=read16(bytes,0); result.entry_count=read16(bytes,2); result.palette_count=read16(bytes,4);
    if(result.version>1)return SfntStatus::unsupported;
    const u32 colors=read16(bytes,6),start_bytes=result.palette_count*2u;
    if(!result.entry_count || !result.palette_count || !colors)return SfntStatus::malformed;
    if(!range(bytes,12,start_bytes+(result.version?12u:0u)))return SfntStatus::truncated;
    u32 at=0; status=reference(bytes,0,read32(bytes,8),colors*4,&at);
    if(status!=SfntStatus::ok)return status;
    result.colors=view(bytes,at,colors*4);result.starts=view(bytes,12,start_bytes);
    for(u32 i=0;i<result.palette_count;++i) {
        const u32 start=read16(result.starts,i*2);
        if(start>colors || result.entry_count>colors-start)return SfntStatus::malformed;
    }
    if(result.version) {
        ConstSlice<u8>* targets[]{&result.types,&result.labels,&result.entry_labels};
        const u32 lengths[]{result.palette_count*4u,result.palette_count*2u,result.entry_count*2u};
        for(u32 i=0;i<3;++i) {
            const u32 offset=read32(bytes,12+start_bytes+i*4);
            if(!offset)continue;
            if(!range(bytes,offset,lengths[i]))return SfntStatus::truncated;
            *targets[i]=view(bytes,offset,lengths[i]);
        }
    }
    *output=result;return SfntStatus::ok;
}

SfntStatus cpal_color(const CpalFont& font,u16 palette,u16 entry,CpalColor* output) noexcept {
    if(!output)return SfntStatus::invalid_input;
    *output={};
    if(!font.starts.data || !font.colors.data)return SfntStatus::invalid_input;
    if(palette>=font.palette_count)return SfntStatus::invalid_input;
    if(entry==0xffff)return SfntStatus::unsupported_sequence; // Application foreground.
    if(entry>=font.entry_count)return SfntStatus::invalid_input;
    const u32 at=(read16(font.starts,palette*2u)+entry)*4u;
    *output={font.colors[at+2],font.colors[at+1],font.colors[at],font.colors[at+3]};
    return SfntStatus::ok;
}
SfntStatus cpal_palette_info(const CpalFont& font,u16 palette,CpalPaletteInfo* output) noexcept {
    if(!output)return SfntStatus::invalid_input;
    *output={0,0xffff};
    if(!font.starts.data || palette>=font.palette_count)return SfntStatus::invalid_input;
    if(font.types.data)output->flags=read32(font.types,palette*4u);
    if(font.labels.data)output->name_id=read16(font.labels,palette*2u);
    return SfntStatus::ok;
}
SfntStatus cpal_entry_label(const CpalFont& font,u16 entry,u16* name_id) noexcept {
    if(!name_id)return SfntStatus::invalid_input;
    *name_id=0xffff;
    if(!font.starts.data || entry>=font.entry_count)return SfntStatus::invalid_input;
    if(font.entry_labels.data)*name_id=read16(font.entry_labels,entry*2u);
    return SfntStatus::ok;
}
} // namespace maicord
