#include "maicord/text/gpos_execute.hpp"
#include "maicord/text/base.hpp"
#include <emmintrin.h>
namespace maicord {namespace {
bool finite(double x) noexcept {u64 bits=0;copy_bytes(&bits,&x,8);return (bits&0x7ff0000000000000ull)!=0x7ff0000000000000ull;}
bool finite(float x) noexcept {u32 bits=0;copy_bytes(&bits,&x,4);return (bits&0x7f800000u)!=0x7f800000u;}
bool valid(const SfntPosition& p) noexcept {return finite(p.advance)&&finite(p.advance_offset)&&finite(p.ascender_offset);}
bool spend(SfntPositionExecution* result,u64* remaining,u64 cost=1) noexcept {if(cost>*remaining)return false;*remaining-=cost;result->operations+=cost;return true;}
SfntStatus resolve(const SfntValueRecord& record,const SfntPositionInstance& instance,SfntPositionAdjustment* out,SfntPositionExecution* result,u64* work) noexcept {
    if(!spend(result,work,9))return SfntStatus::resource_limit;
    if(!record.format){*out={};return SfntStatus::ok;}
    if(!(record.format&0xf0)){
        *out={};const u8* at=record.parent.data+record.offset;
        for(u32 i=0;i<4;++i)if(record.format&(1u<<i)){out->design[i]=static_cast<i16>(byte_load_be16(at));at+=2;}
        return SfntStatus::ok;
    }
    SfntPositionValue value{};auto s=sfnt_value_record_read(record,&value);if(s!=SfntStatus::ok)return s;
    if(instance.variable)for(u32 i=0;i<4;++i){u32 index=0;if(!sfnt_device_variation_index(value.devices[i],&index) || index==0xffffffffu)continue;
        if(!instance.variations)return SfntStatus::missing_table;ItemVariationData data{};s=item_variation_data(instance.variations->store,static_cast<u16>(index>>16),&data);if(s!=SfntStatus::ok)return s;
        if(!spend(result,work,static_cast<u64>(data.region_count)+1))return SfntStatus::resource_limit;}
    return sfnt_position_value_resolve(value,instance,out);
}
SfntPosition adjusted(const SfntPosition& p,const SfntPositionAdjustment& v,bool rtl) noexcept {
    return {p.advance+v.design[2],p.advance_offset+(rtl?v.design[2]-v.design[0]:v.design[0]),p.ascender_offset+v.design[1]};
}
float scaled(double value,__m128 scale) noexcept {
    const __m128 v=_mm_mul_ss(_mm_set_ss(static_cast<float>(value)),scale);
    return _mm_cvtss_f32(_mm_andnot_ps(_mm_cmpeq_ss(v,_mm_setzero_ps()),v));
}
}
SfntStatus sfnt_trailing_spacing(Slice<SfntPlacement> placements,
    ConstSlice<u8> flags,float trailing,u64 work,bool rtl) noexcept {
    u32 bits=0;copy_bytes(&bits,&trailing,4);
    if((bits&0x7fffffffu)>=0x7f800000u||((bits&0x80000000u)&&(bits&0x7fffffffu))||(placements.count&&!placements.data)||
       (flags.count&&(flags.count!=placements.count||!flags.data)))return SfntStatus::invalid_input;
    if(placements.count>work)return SfntStatus::resource_limit;
    if(!trailing)return SfntStatus::ok;
    const auto spacing=_mm_set_ss(trailing);
    if(!flags.count){for(u32 i=0;i<placements.count;++i){
        const float value=_mm_cvtss_f32(_mm_add_ss(_mm_set_ss(placements[i].advance),spacing));
        if(!finite(value))return SfntStatus::resource_limit;placements[i].advance=value;
        if(rtl){const float offset=_mm_cvtss_f32(_mm_add_ss(_mm_set_ss(placements[i].advance_offset),spacing));
            if(!finite(offset))return SfntStatus::resource_limit;placements[i].advance_offset=offset;}}return SfntStatus::ok;}
    if(placements.count&&!(flags[0]&sfnt_position_cluster_start))return SfntStatus::invalid_input;
    for(u32 first=0;first<placements.count;){
        u32 end=first,last=0xffffffffu;
        do{if(!(flags[end]&sfnt_position_zero_advance)&&(!rtl||last==0xffffffffu))last=end;++end;}while(end<placements.count&&!(flags[end]&sfnt_position_cluster_start));
        if(last!=0xffffffffu){const float advance=_mm_cvtss_f32(_mm_add_ss(_mm_set_ss(placements[last].advance),spacing));
            if(!finite(advance))return SfntStatus::resource_limit;placements[last].advance=advance;
            const u32 begin=rtl?first:last+1,limit=rtl?last+1:end;
            for(u32 i=begin;i<limit;++i){const __m128 original=_mm_set_ss(placements[i].advance_offset);
                const float offset=_mm_cvtss_f32(rtl?_mm_add_ss(original,spacing):_mm_sub_ss(original,spacing));
                if(!finite(offset))return SfntStatus::resource_limit;placements[i].advance_offset=offset;}}
        first=end;
    }
    return SfntStatus::ok;
}
SfntStatus sfnt_position_initialize(ConstSlice<double> advances,ConstSlice<u8> flags,bool rtl,Slice<SfntPosition> output,u64 work) noexcept {
    if(advances.count!=flags.count || (advances.count && (!advances.data || !flags.data)))return SfntStatus::invalid_input;if(output.count<advances.count)return SfntStatus::output_too_small;
    if(advances.count && !output.data)return SfntStatus::invalid_input;if(advances.count>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<advances.count;++i)if(!finite(advances.data[i]) || (flags.data[i]&~3u) || (!i && !(flags.data[i]&sfnt_position_cluster_start)))return SfntStatus::invalid_input;
    for(u32 i=0;i<advances.count;++i){const bool zero=(flags.data[i]&sfnt_position_zero_advance)!=0;output.data[i]={zero?0:advances.data[i],zero&&rtl&&advances.data[i]!=0?-advances.data[i]:0,0};}return SfntStatus::ok;
}
SfntStatus sfnt_position_execute_lookup(const SfntGposLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,Slice<SfntPosition> positions,u32 start,u32 end,bool rtl,const SfntPositionInstance& instance,SfntPositionExecution* result,u64 work,u32 match_limit,SfntPositionApplication application) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};if(!sfnt_glyph_view_valid(glyphs) || start>end || end>sfnt_glyph_count(glyphs) || positions.count<sfnt_glyph_count(glyphs) || (positions.count && !positions.data) || static_cast<u8>(instance.arithmetic)>1)return SfntStatus::invalid_input;
    if(static_cast<u8>(application)>1)return SfntStatus::invalid_input;const bool selected=application==SfntPositionApplication::selected;
    for(u32 at=start;at<end && (!selected || at==start);){if(!spend(result,&work))return SfntStatus::resource_limit;const auto& glyph=sfnt_glyph_at(glyphs,at);
        if(static_cast<u8>(glyph.glyph_class)>4)return SfntStatus::invalid_input;if(lookup.subtables.count && glyph.glyph>=lookup.subtables.data[0].glyph_limit)return SfntStatus::glyph_out_of_range;
        if(!selected && sfnt_lookup_ignores(gdef,lookup.filter,glyph.glyph,glyph.glyph_class)){++at;continue;}
        SfntGposMatch match{};
        for(u32 i=0;i<lookup.subtables.count;++i){u32 used=0;const u32 budget=work<match_limit?static_cast<u32>(work):match_limit;
            const auto s=sfnt_gpos_subtable_match_view(lookup.subtables.data[i],gdef,lookup.filter,glyphs,at,end,&match,budget,&used);spend(result,&work,used);if(s!=SfntStatus::ok)return s;if(match.matched)break;}
        if(!match.matched){++at;continue;}
        SfntPositionAdjustment first{},second{};auto s=resolve(match.values[0],instance,&first,result,&work);if(s!=SfntStatus::ok)return s;
        const auto a=adjusted(positions.data[at],first,rtl);SfntPosition b{};const bool pair=match.second_input!=0xffffffffu;
        if(pair){s=resolve(match.values[1],instance,&second,result,&work);if(s!=SfntStatus::ok)return s;b=adjusted(positions.data[match.second_input],second,rtl);}
        if(!valid(a) || (pair && !valid(b)))return SfntStatus::resource_limit;
        positions.data[at]=a;if(pair)positions.data[match.second_input]=b;++result->matches;result->adjusted_glyphs+=1+(pair?1u:0u);at=match.next_input;
    }return SfntStatus::ok;
}
SfntStatus sfnt_position_finalize(ConstSlice<SfntPosition> positions,ConstSlice<u8> flags,float em,u16 upem,Slice<SfntPlacement> output,u64 work,bool consecutive_zeroes,bool retain_zero_positioning) noexcept {
    u32 bits=0;copy_bytes(&bits,&em,4);if(!bits || bits>=0x7f800000u || !upem || positions.count!=flags.count || (positions.count && (!positions.data || !flags.data)))return SfntStatus::invalid_input;
    if(output.count<positions.count)return SfntStatus::output_too_small;if(positions.count && !output.data)return SfntStatus::invalid_input;if(positions.count>work)return SfntStatus::resource_limit;
    const __m128 scale=_mm_div_ss(_mm_set_ss(em),_mm_set_ss(static_cast<float>(upem)));double removed=0;
    for(u32 i=0;i<positions.count;++i){const auto& p=positions.data[i];const u8 f=flags.data[i];if((f&~3u) || (!i && !(f&sfnt_position_cluster_start)) || !valid(p))return SfntStatus::invalid_input;
        const bool zero=(f&sfnt_position_zero_advance)!=0&&!retain_zero_positioning;if(consecutive_zeroes?!zero:(f&sfnt_position_cluster_start)!=0)removed=0;
        const double x=p.advance_offset+removed;if(!finite(x))return SfntStatus::resource_limit;const SfntPlacement value{scaled(zero?0:p.advance,scale),scaled(x,scale),scaled(p.ascender_offset,scale)};
        if(!finite(value.advance)||!finite(value.advance_offset)||!finite(value.ascender_offset))return SfntStatus::resource_limit;output.data[i]=value;
        if(zero){removed+=p.advance;if(!finite(removed))return SfntStatus::resource_limit;}
    }return SfntStatus::ok;
}
}
