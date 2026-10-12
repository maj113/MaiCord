#include "color_internal.hpp"

namespace maicord {
using namespace color_detail;
namespace {
constexpr u8 sizes[]{0,6,5,9,16,20,16,20,12,16,6,3,7,7,8,12,8,12,12,16,6,10,10,14,6,10,10,14,8,12,12,16,8};

SfntStatus read_paint(const ColrFont& font,u32 at,ColrPaint* p) noexcept {
    const auto t=font.table;
    if(!range(t,at,1))return SfntStatus::truncated;
    const u8 format=t[at];
    if(!format || format>32)return SfntStatus::unsupported;
    if(!range(t,at,sizes[format]))return SfntStatus::truncated;
    p->offset=at;p->format=format;p->variation_index=0xffffffff;
    p->variable=(format==3 || format==5 || format==7 || format==9 || (format>=13 && format<=31 && (format&1)));
    if(format==1) {
        p->layer_count=t[at+1];p->first_layer=read32(t,at+2);
        const u32 count=font.layers_v1.count/4;
        return p->first_layer<=count && p->layer_count<=count-p->first_layer?SfntStatus::ok:SfntStatus::malformed;
    }
    if(format==2 || format==3) {
        p->palette=read16(t,at+1);p->value_count=1;p->values[0]=signed16(t,at+3)/16384.0;
        if(p->variable)p->variation_index=read32(t,at+5);
        return palette_valid(font,p->palette)?SfntStatus::ok:SfntStatus::malformed;
    }
    if(format>=4 && format<=9) {
        auto status=reference(t,at,read24(t,at+1),3,&p->color_line);if(status!=SfntStatus::ok)return status;
        p->value_count=format<=7?6:4;
        for(u32 i=0;i<p->value_count;++i) {
            const u32 field=at+4+i*2;
            p->values[i]=format>=6 && format<=7 && (i==2 || i==5)?read16(t,field):signed16(t,field);
            if(format>=8 && i>=2)p->values[i]/=16384.0;
        }
        if(p->variable)p->variation_index=read32(t,at+4+p->value_count*2);
        return SfntStatus::ok;
    }
    if(format==11) {
        p->glyph=read16(t,at+1);
        return colr_base_paint(font,p->glyph,&p->child[0]);
    }
    auto status=reference(t,at,read24(t,at+1),1,&p->child[0]);if(status!=SfntStatus::ok)return status;
    if(format==10) {p->glyph=read16(t,at+4);return p->glyph<font.glyph_count?SfntStatus::ok:SfntStatus::glyph_out_of_range;}
    if(format==32) {
        p->mode=t[at+4];if(p->mode>27)return SfntStatus::malformed;
        return reference(t,at,read24(t,at+5),1,&p->child[1]);
    }
    if(format==12 || format==13) {
        u32 transform=0;status=reference(t,at,read24(t,at+4),p->variable?28u:24u,&transform);
        if(status!=SfntStatus::ok)return status;
        p->value_count=6;
        for(u32 i=0;i<6;++i)p->values[i]=static_cast<i32>(read32(t,transform+i*4))/65536.0;
        if(p->variable)p->variation_index=read32(t,transform+24);
        return SfntStatus::ok;
    }
    // Compact transform families share a child and consecutive 16-bit fields.
    constexpr u8 counts[]{2,2,4,1,3,1,3,2,4};
    constexpr u8 fractional_masks[]{0,3,3,1,1,1,1,3,3};
    const u32 family=(format-14)/2;
    p->value_count=counts[family];
    for(u32 i=0;i<p->value_count;++i) {
        p->values[i]=signed16(t,at+4+i*2);
        if(fractional_masks[family]&(1u<<i))p->values[i]/=16384.0;
    }
    if(p->variable)p->variation_index=read32(t,at+4+p->value_count*2);
    return SfntStatus::ok;
}
}

SfntStatus colr_paint_read(const ColrFont& font,u32 offset,ColrPaint* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!font.table.data)return SfntStatus::invalid_input;
    const auto status=read_paint(font,offset,output);
    if(status!=SfntStatus::ok)*output={};
    return status;
}
SfntStatus colr_color_line(const ColrFont& font,const ColrPaint& paint,ColrColorLine* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!font.table.data || paint.format<4 || paint.format>9)return SfntStatus::invalid_input;
    const u32 at=paint.color_line;
    if(!range(font.table,at,3))return SfntStatus::truncated;
    const u8 extend=font.table[at];const u16 count=read16(font.table,at+1);
    if(extend>2)return SfntStatus::malformed;
    const u32 length=count*(paint.variable?10u:6u);
    if(!range(font.table,at+3,length))return SfntStatus::truncated;
    *output={view(font.table,at+3,length),count,extend,paint.variable};return SfntStatus::ok;
}
SfntStatus colr_color_stop(const ColrFont& font,const ColrColorLine& line,u32 index,ColrColorStop* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!font.table.data || index>=line.count)return SfntStatus::invalid_input;
    const u32 stride=line.variable?10u:6u,at=index*stride;
    if(!range(line.stops,at,stride))return SfntStatus::truncated;
    const u16 palette=read16(line.stops,at+2);
    if(!palette_valid(font,palette))return SfntStatus::malformed;
    *output={signed16(line.stops,at)/16384.0,signed16(line.stops,at+4)/16384.0,
             line.variable?read32(line.stops,at+6):0xffffffffu,palette,line.variable};return SfntStatus::ok;
}
} // namespace maicord
