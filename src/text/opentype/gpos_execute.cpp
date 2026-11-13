#include "maicord/text/opentype/gpos_execute.hpp"
#include "maicord/core/memory.hpp"
#include "gpos_validated.hpp"
#include "maicord/sys/simd.hpp"
namespace maicord {namespace {
inline bool valid(const SfntPosition& p) noexcept {return is_finite(p.advance)&&is_finite(p.advance_offset)&&is_finite(p.ascender_offset);}
inline bool spend(SfntPositionExecution* result,u64* remaining,u64 cost=1) noexcept {if(cost>*remaining)return false;*remaining-=cost;result->operations+=cost;return true;}
SfntStatus resolve(const SfntValueRecord& record,const SfntPositionInstance& instance,SfntPositionAdjustment* out,SfntPositionExecution* result,u64* work) noexcept {
    if(!spend(result,work,9))return SfntStatus::resource_limit;
    if(!record.format){reset_value(out);return SfntStatus::ok;}
    if(!(record.format&0xf0)){
        reset_value(out);const u8* at=record.parent.data+record.offset;
        for(u32 i=0;i<4;++i)if(record.format&(1u<<i)){out->design[i]=static_cast<i16>(byte_load_be16(at));at+=2;}
        return SfntStatus::ok;
    }
    SfntPositionValue value{};auto s=sfnt_value_record_read(record,&value);if(s!=SfntStatus::ok)return s;
    if(instance.variable)for(u32 i=0;i<4;++i){u32 index=0;if(!sfnt_device_variation_index(value.devices[i],&index) || index==0xffffffffu)continue;
        if(!instance.variations)return SfntStatus::missing_table;ItemVariationData data{};s=item_variation_data(instance.variations->store,static_cast<u16>(index>>16),&data);if(s!=SfntStatus::ok)return s;
        if(!spend(result,work,static_cast<u64>(data.region_count)+1))return SfntStatus::resource_limit;}
    return sfnt_position_value_resolve(value,instance,out);
}
// A value record's placement moves the glyph right; right to left the offset is stored negated (the renderer draws
// at pen - advance - offset), so an advance change leaves the glyph's placement alone, as in HarfBuzz.
inline SfntPosition adjusted(const SfntPosition& p,const SfntPositionAdjustment& v,bool rtl) noexcept {
    return {p.advance+v.design[2],p.advance_offset+(rtl?-v.design[0]:v.design[0]),p.ascender_offset+v.design[1]};
}
inline float scaled(double value,__m128 scale) noexcept {
    const __m128 v=_mm_mul_ss(_mm_set_ss(static_cast<float>(value)),scale);
    return _mm_cvtss_f32(_mm_andnot_ps(_mm_cmpeq_ss(v,_mm_setzero_ps()),v));
}
// Visits each horizontal, not cross-stream format-0 pair list of a legacy kern table: visit(pairs,count). False when
// a header leaves the table; the last list may outgrow its 16-bit length (large tables), the table end bounds it.
template<class Visit> bool kern_lists(ConstSlice<u8> table,Visit visit) noexcept {
    if(table.count<4||byte_load_be16(table.data))return false;const u32 count=byte_load_be16(table.data+2);u32 at=4;bool found=false;
    for(u32 i=0;i<count;++i){if(table.count-at<6)return false;const u32 length=byte_load_be16(table.data+at+2),coverage=byte_load_be16(table.data+at+4);
        if((coverage&0xff05u)==1){if(table.count-at<14)return false;const u32 pairs=byte_load_be16(table.data+at+6),end=i+1==count?table.count:at+length;
            if(end>table.count||at+14+pairs*6>end)return false;visit(table.data+at+14,pairs);found=true;}
        if(length<6)return false;at+=length;}
    return found;
}
i32 kern_value(ConstSlice<u8> table,u16 left,u16 right) noexcept {
    const u32 key=static_cast<u32>(left)<<16|right;i32 sum=0;
    kern_lists(table,[&](const u8* pairs,u32 count) noexcept {u32 lo=0,hi=count;
        while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(byte_load_be32(pairs+mid*6)<key)lo=mid+1;else hi=mid;}
        if(lo<count&&byte_load_be32(pairs+lo*6)==key)sum+=static_cast<i16>(byte_load_be16(pairs+lo*6+4));});
    return sum;
}
}
bool sfnt_legacy_kern_open(ConstSlice<u8> table,Slice<u64> bitmap) noexcept {
    if(!table.data||!kern_lists(table,[](const u8*,u32) noexcept {}))return false;
    kern_lists(table,[&](const u8* pairs,u32 count) noexcept {
        for(u32 i=0;i<count;++i)for(u32 side=0;side<2;++side){const u32 glyph=byte_load_be16(pairs+i*6+side*2);
            if(glyph/64<bitmap.count)bitmap.data[glyph/64]|=1ull<<(glyph%64);}});
    return true;
}
SfntStatus sfnt_legacy_kern_apply(ConstSlice<u8> table,SfntGlyphView glyphs,ConstSlice<u8> flags,Slice<SfntPosition> positions,bool rtl,u64* work) noexcept {
    const u32 count=sfnt_glyph_count(glyphs);if(flags.count<count||positions.count<count)return SfntStatus::invalid_input;
    for(u32 i=0;i<count;){u32 j=i+1;while(j<count&&(flags.data[j]&sfnt_position_zero_advance))++j;if(j==count)break;
        if(!*work)return SfntStatus::resource_limit;--*work;
        const i32 kern=kern_value(table,sfnt_glyph_at(glyphs,i).glyph,sfnt_glyph_at(glyphs,j).glyph);
        if(kern){const i32 first=kern>>1,second=kern-first;
            positions.data[i]=adjusted(positions.data[i],{{0,0,static_cast<double>(first),0}},rtl);
            positions.data[j]=adjusted(positions.data[j],{{static_cast<double>(second),0,static_cast<double>(second),0}},rtl);}
        i=j;}
    return SfntStatus::ok;
}
bool sfnt_trailing_spacing_valid(float spacing) noexcept {
    u32 bits=0;copy_bytes(&bits,&spacing,4);return (bits&0x7fffffffu)<0x7f800000u&&(!(bits&0x80000000u)||!(bits&0x7fffffffu));
}
SfntStatus sfnt_trailing_spacing(Slice<SfntPlacement> placements,
    ConstSlice<u8> flags,float trailing,u64 work,bool rtl) noexcept {
    if(!sfnt_trailing_spacing_valid(trailing)||(placements.count&&!placements.data)||
       (flags.count&&(flags.count!=placements.count||!flags.data)))return SfntStatus::invalid_input;
    if(placements.count>work)return SfntStatus::resource_limit;
    if(!trailing)return SfntStatus::ok;
    const auto spacing=_mm_set_ss(trailing);
    if(!flags.count){for(u32 i=0;i<placements.count;++i){
        const float value=_mm_cvtss_f32(_mm_add_ss(_mm_set_ss(placements[i].advance),spacing));
        if(!is_finite(value))return SfntStatus::resource_limit;placements[i].advance=value;
        if(rtl){const float offset=_mm_cvtss_f32(_mm_add_ss(_mm_set_ss(placements[i].advance_offset),spacing));
            if(!is_finite(offset))return SfntStatus::resource_limit;placements[i].advance_offset=offset;}}return SfntStatus::ok;}
    if(placements.count&&!(flags[0]&sfnt_position_cluster_start))return SfntStatus::invalid_input;
    for(u32 first=0;first<placements.count;){
        u32 end=first,last=0xffffffffu;
        do{if(!(flags[end]&sfnt_position_zero_advance)&&(!rtl||last==0xffffffffu))last=end;++end;}while(end<placements.count&&!(flags[end]&sfnt_position_cluster_start));
        if(last!=0xffffffffu){const float advance=_mm_cvtss_f32(_mm_add_ss(_mm_set_ss(placements[last].advance),spacing));
            if(!is_finite(advance))return SfntStatus::resource_limit;placements[last].advance=advance;
            const u32 begin=rtl?first:last+1,limit=rtl?last+1:end;
            for(u32 i=begin;i<limit;++i){const __m128 original=_mm_set_ss(placements[i].advance_offset);
                const float offset=_mm_cvtss_f32(rtl?_mm_add_ss(original,spacing):_mm_sub_ss(original,spacing));
                if(!is_finite(offset))return SfntStatus::resource_limit;placements[i].advance_offset=offset;}}
        first=end;
    }
    return SfntStatus::ok;
}
SfntStatus sfnt_position_execute_lookup(const SfntGposLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,Slice<SfntPosition> positions,u32 start,u32 end,bool rtl,const SfntPositionInstance& instance,SfntPositionExecution* result,u64 work,u32 match_limit,SfntPositionApplication application,const SfntLookupBounds* bounds) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);if(!sfnt_glyph_view_valid(glyphs) || start>end || end>sfnt_glyph_count(glyphs) || positions.count<sfnt_glyph_count(glyphs) || (positions.count && !positions.data) || static_cast<u8>(instance.arithmetic)>1)return SfntStatus::invalid_input;
    if(static_cast<u8>(application)>1)return SfntStatus::invalid_input;const bool selected=application==SfntPositionApplication::selected;
    for(u32 at=start;at<end && (!selected || at==start);){if(!spend(result,&work))return SfntStatus::resource_limit;const auto& glyph=sfnt_glyph_at(glyphs,at);
        if(static_cast<u8>(glyph.glyph_class)>4)return SfntStatus::invalid_input;if(lookup.subtables.count && glyph.glyph>=lookup.subtables.data[0].glyph_limit)return SfntStatus::glyph_out_of_range;
        if(!selected && ((bounds&&sfnt_lookup_bounds_exclude(*bounds,glyph.glyph))||sfnt_lookup_ignores(gdef,lookup.filter,glyph.glyph,glyph.glyph_class))){++at;continue;}
        SfntGposMatch match{};
        for(u32 i=0;i<lookup.subtables.count;++i){const u32 budget=work<match_limit?static_cast<u32>(work):match_limit;
            u32 left=budget;const auto s=gpos_subtable_match(lookup.subtables.data[i],gdef,lookup.filter,glyphs,at,end,&match,&left);spend(result,&work,budget-left);if(s!=SfntStatus::ok)return s;if(match.matched)break;}
        if(!match.matched){++at;continue;}
        SfntPositionAdjustment first{},second{};auto s=resolve(match.values[0],instance,&first,result,&work);if(s!=SfntStatus::ok)return s;
        const auto a=adjusted(positions.data[at],first,rtl);SfntPosition b{};const bool pair=match.second_input!=0xffffffffu;
        if(pair){s=resolve(match.values[1],instance,&second,result,&work);if(s!=SfntStatus::ok)return s;b=adjusted(positions.data[match.second_input],second,rtl);}
        if(!valid(a) || (pair && !valid(b)))return SfntStatus::resource_limit;
        positions.data[at]=a;if(pair)positions.data[match.second_input]=b;++result->matches;at=match.next_input;
    }return SfntStatus::ok;
}
// A zero-advance glyph keeps its offset: HarfBuzz zeroes mark advances without moving any glyph
// (zero_mark_widths_by_gdef when GPOS is present).
SfntStatus sfnt_position_finalize(ConstSlice<SfntPosition> positions,ConstSlice<u8> flags,float em,u16 upem,Slice<SfntPlacement> output,u64 work,bool retain_zero_positioning) noexcept {
    u32 bits=0;copy_bytes(&bits,&em,4);if(!bits || bits>=0x7f800000u || !upem || positions.count!=flags.count || (positions.count && (!positions.data || !flags.data)))return SfntStatus::invalid_input;
    if(output.count<positions.count)return SfntStatus::output_too_small;if(positions.count && !output.data)return SfntStatus::invalid_input;if(positions.count>work)return SfntStatus::resource_limit;
    const __m128 scale=_mm_div_ss(_mm_set_ss(em),_mm_set_ss(static_cast<float>(upem)));
    for(u32 i=0;i<positions.count;++i){const auto& p=positions.data[i];const u8 f=flags.data[i];if((f&~3u) || (!i && !(f&sfnt_position_cluster_start)) || !valid(p))return SfntStatus::invalid_input;
        const bool zero=(f&sfnt_position_zero_advance)!=0&&!retain_zero_positioning;
        const SfntPlacement value{scaled(zero?0:p.advance,scale),scaled(p.advance_offset,scale),scaled(p.ascender_offset,scale)};
        if(!is_finite(value.advance)||!is_finite(value.advance_offset)||!is_finite(value.ascender_offset))return SfntStatus::resource_limit;output.data[i]=value;
    }return SfntStatus::ok;
}
}
