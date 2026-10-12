#include "maicord/text/nominal_ranges.hpp"
#include "maicord/text/nominal_sources.hpp"
namespace maicord {namespace {
#include "generated/nominal_range_data.inc"
bool scalar(u32 cp) noexcept {return cp<=0x10ffff&&(cp<0xd800||cp>0xdfff);}
// Packed start (21 bits), interior-backtracking flag, and length minus one.
u32 attachment(u32 cp) noexcept {
    u32 first=0,last=sizeof(nominal_attachment_ranges)/sizeof(u32);
    while(first<last){const u32 middle=first+(last-first)/2;if((nominal_attachment_ranges[middle]>>9)<=cp)first=middle+1;else last=middle;}
    if(!first)return 0;const u32 record=nominal_attachment_ranges[first-1];
    return cp-(record>>9)<=(record&255)?1u|((record&256)?2u:0u):0;
}
struct Scan {
    ConstSlice<u32> scalars;SfntGlyphView glyphs;ConstSlice<u64> participation;u32 limit,work;SfntNominalRange& result;
    SfntStatus test(u32 at,bool lookahead,bool* simple) noexcept {
        *simple=false;if(!work)return SfntStatus::resource_limit;--work;++result.examined;
        const u32 cp=scalars[at];if(!scalar(cp))return SfntStatus::invalid_input;
        const u32 glyph=sfnt_glyph_at(glyphs,at).glyph;if(glyph>=limit)return SfntStatus::glyph_out_of_range;
        if(!glyph||!nominal_scalar_is_simple(cp)||(participation.count&&(participation[glyph/64]&(1ull<<(glyph%64)))))return SfntStatus::ok;
        if(lookahead&&at+1<scalars.count){const u32 next=scalars[at+1];if(!scalar(next))return SfntStatus::invalid_input;if(attachment(next)&1)return SfntStatus::ok;}
        *simple=true;return SfntStatus::ok;
    }
};
}
SfntStatus sfnt_nominal_range(ConstSlice<u32> scalars,SfntGlyphView glyphs,ConstSlice<u64> participation,
    u32 limit,SfntNominalRange* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if((scalars.count&&!scalars.data)||scalars.count>0x7fffffffu||!sfnt_glyph_view_valid(glyphs)||
       sfnt_glyph_count(glyphs)!=scalars.count||!limit||limit>65536||
       (participation.count&&(!participation.data||participation.count<(limit+63)/64)))return SfntStatus::invalid_input;
    if(!scalars.count)return SfntStatus::ok;
    Scan scan{scalars,glyphs,participation,limit,work,*output};bool simple=false;
    auto status=scan.test(0,true,&simple);if(status!=SfntStatus::ok)return status;
    u32 end=1;
    while(end<scalars.count){bool next=false;status=scan.test(end,!simple,&next);if(status!=SfntStatus::ok)return status;
        if(next!=simple)break;++end;
    }
    if(simple&&end<scalars.count&&(attachment(scalars[end])&2))--end;
    if(!end)return SfntStatus::malformed;
    u32 units=0;for(u32 i=0;i<end;++i)units+=scalars[i]>65535?2:1;
    output->scalar_count=end;output->utf16_units=units;output->simple=simple;return SfntStatus::ok;
}
}
