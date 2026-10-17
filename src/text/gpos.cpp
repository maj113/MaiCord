#include "maicord/text/gpos.hpp"
#include "maicord/text/base.hpp"

namespace maicord {namespace {
struct WorkReport {u32 initial;const u32& remaining;u32* used;~WorkReport() noexcept {if(used)*used=initial-remaining;}};
inline bool spend(u32* work,u32 amount=1) noexcept {if(amount>*work)return false;*work-=amount;return true;}
inline u16 r16(ConstSlice<u8> b,u32 at) noexcept {return byte_load_be16(b.data+at);}
bool span(ConstSlice<u8> b,u32 at,u64 size) noexcept {return at<=b.count && size<=b.count-at && (!size || b.data);}
SfntStatus tail(ConstSlice<u8> b,u32 at,u32 minimum,ConstSlice<u8>* output) noexcept {
    *output={};if(at<minimum)return SfntStatus::malformed;if(!span(b,at,2))return SfntStatus::truncated;*output={b.data+at,b.count-at};return SfntStatus::ok;
}
SfntStatus device(ConstSlice<u8> parent,u16 offset,u32 minimum,SfntDevice* output,u32* work) noexcept {
    *output={};if(!offset)return SfntStatus::ok;if(!spend(work))return SfntStatus::resource_limit;ConstSlice<u8> bytes{};auto s=tail(parent,offset,minimum,&bytes);return s==SfntStatus::ok?sfnt_device_open(bytes,output):s;
}
SfntStatus value_valid(ConstSlice<u8> parent,u32 offset,u16 format,u32 minimum,u32* work) noexcept {
    const u16 size=sfnt_position_value_size(format);if(size==0xffff)return SfntStatus::unsupported;
    if((parent.count && !parent.data) || minimum>parent.count)return SfntStatus::invalid_input;if(!span(parent,offset,size))return SfntStatus::truncated;
    if(!spend(work))return SfntStatus::resource_limit;
    u32 at=offset;for(u32 i=0;i<8;++i)if(format&(1u<<i)){if(i>=4){SfntDevice d{};auto s=device(parent,r16(parent,at),minimum,&d,work);if(s!=SfntStatus::ok)return s;}at+=2;}
    return SfntStatus::ok;
}
inline SfntValueRecord value_view(ConstSlice<u8> parent,u32 offset,u8 format,u8 bytes) noexcept {return {parent,offset,format,bytes};}
SfntStatus coverage(ConstSlice<u8> parent,u16 offset,u32 minimum,u32 limit,SfntCoverage* output,u32* work) noexcept {
    ConstSlice<u8> bytes{};auto s=tail(parent,offset,minimum,&bytes);if(s!=SfntStatus::ok)return s;if(!span(bytes,0,4))return SfntStatus::truncated;
    if(!spend(work,r16(bytes,2)))return SfntStatus::resource_limit;return sfnt_coverage_open(bytes,limit,output);
}
SfntStatus classes(ConstSlice<u8> parent,u16 offset,u32 minimum,u32 limit,SfntClassDef* output,u32* work) noexcept {
    ConstSlice<u8> bytes{};auto s=tail(parent,offset,minimum,&bytes);if(s!=SfntStatus::ok)return s;if(!span(bytes,0,4))return SfntStatus::truncated;
    if(r16(bytes,0)==1 && !span(bytes,0,6))return SfntStatus::truncated;const u32 count=r16(bytes,r16(bytes,0)==1?4:2);
    if(!spend(work,count))return SfntStatus::resource_limit;return sfnt_class_def_open(bytes,limit,output);
}
SfntStatus validate_values(ConstSlice<u8> parent,u32 at,u32 count,u32 stride,u8 format,u32 minimum,u32* work) noexcept {
    // Every scalar bit pattern is valid; only device offsets require traversal.
    if(!(format&0xf0))return SfntStatus::ok;
    for(u32 i=0;i<count;++i){auto s=value_valid(parent,at+i*stride,format,minimum,work);if(s!=SfntStatus::ok)return s;}return SfntStatus::ok;
}
SfntStatus open(ConstSlice<u8> bytes,u16 type,u32 limit,SfntGposSubtable* output,u32* work) noexcept {
    *output={};if(!bytes.data || !limit || limit>65536)return SfntStatus::invalid_input;if(type<1 || type>2)return SfntStatus::unsupported;
    if(!spend(work))return SfntStatus::resource_limit;if(!span(bytes,0,type==1?6u:10u))return SfntStatus::truncated;const u16 fmt=r16(bytes,0);if(fmt<1 || fmt>2)return SfntStatus::unsupported;
    const u16 vf1=r16(bytes,4),vf2=type==2?r16(bytes,6):0,b1=sfnt_position_value_size(vf1),b2=sfnt_position_value_size(vf2);if(b1==0xffff || b2==0xffff)return SfntStatus::unsupported;
    SfntGposSubtable result{};result.table=bytes;result.glyph_limit=limit;result.type=static_cast<u8>(type);result.format=static_cast<u8>(fmt);
    result.value_formats[0]=static_cast<u8>(vf1);result.value_formats[1]=static_cast<u8>(vf2);result.value_bytes[0]=static_cast<u8>(b1);result.value_bytes[1]=static_cast<u8>(b2);
    u32 header=0;SfntStatus s;
    if(type==1){if(fmt==2 && !span(bytes,0,8))return SfntStatus::truncated;const u16 count=fmt==1?1:r16(bytes,6);const u32 at=fmt==1?6u:8u;
        header=at+static_cast<u32>(count)*b1;if(!span(bytes,0,header))return SfntStatus::truncated;
        s=coverage(bytes,r16(bytes,2),header,limit,&result.coverage,work);if(s!=SfntStatus::ok)return s;
        if(fmt==2 && result.coverage.covered_count!=count)return SfntStatus::malformed;result.counts[0]=count;
        s=validate_values(bytes,at,count,b1,static_cast<u8>(vf1),header,work);if(s!=SfntStatus::ok)return s;
    } else if(fmt==1){const u16 count=r16(bytes,8);header=10+static_cast<u32>(count)*2;if(!span(bytes,0,header))return SfntStatus::truncated;
        s=coverage(bytes,r16(bytes,2),header,limit,&result.coverage,work);if(s!=SfntStatus::ok)return s;if(result.coverage.covered_count!=count)return SfntStatus::malformed;
        if(!spend(work,count))return SfntStatus::resource_limit;result.counts[0]=count;
        for(u32 i=0;i<count;++i){ConstSlice<u8> set{};s=tail(bytes,r16(bytes,10+i*2),header,&set);if(s!=SfntStatus::ok)return s;
            const u32 pairs=r16(set,0),stride=2u+b1+b2,end=2+pairs*stride;if(!span(set,0,end))return SfntStatus::truncated;if(!spend(work,pairs))return SfntStatus::resource_limit;
            u16 previous=0;for(u32 j=0;j<pairs;++j){const u16 glyph=r16(set,2+j*stride);if(glyph>=limit || (j && glyph<=previous))return SfntStatus::malformed;previous=glyph;}
            s=validate_values(set,4,pairs,stride,static_cast<u8>(vf1),end,work);if(s!=SfntStatus::ok)return s;
            s=validate_values(set,4+b1,pairs,stride,static_cast<u8>(vf2),end,work);if(s!=SfntStatus::ok)return s;
        }
    } else {if(!span(bytes,0,16))return SfntStatus::truncated;const u16 c1=r16(bytes,12),c2=r16(bytes,14);const u32 stride=static_cast<u32>(b1)+b2;
        const u64 cells=static_cast<u64>(c1)*c2,total=16+cells*stride;if(!span(bytes,0,total))return SfntStatus::truncated;header=static_cast<u32>(total);result.counts[0]=c1;result.counts[1]=c2;
        s=coverage(bytes,r16(bytes,2),header,limit,&result.coverage,work);if(s!=SfntStatus::ok)return s;
        for(u32 i=0;i<2;++i){s=classes(bytes,r16(bytes,8+i*2),header,limit,&result.classes[i],work);if(s!=SfntStatus::ok)return s;}
        s=validate_values(bytes,16,static_cast<u32>(cells),stride,static_cast<u8>(vf1),header,work);if(s!=SfntStatus::ok)return s;
        s=validate_values(bytes,16+b1,static_cast<u32>(cells),stride,static_cast<u8>(vf2),header,work);if(s!=SfntStatus::ok)return s;
    }
    if(result.coverage.record_count){const auto& c=result.coverage;
        result.last_covered_glyph=byte_load_be16(c.records+(c.record_count-1u)*(c.format==1?2u:6u)+(c.format==1?0u:2u));}
    else result.last_covered_glyph=0;
    *output=result;return SfntStatus::ok;
}
inline bool glyph_valid(const SfntGsubGlyph& glyph,u32 limit) noexcept {return glyph.glyph<limit && static_cast<u8>(glyph.glyph_class)<=4;}
SfntStatus match(const SfntGposSubtable& sub,const SfntGdef& gdef,const SfntLookupFilter& filter,SfntGlyphView glyphs,u32 start,u32 end,SfntGposMatch* output,u32* work) noexcept {
    if(!spend(work))return SfntStatus::resource_limit;const auto& first=sfnt_glyph_at(glyphs,start);if(!glyph_valid(first,sub.glyph_limit))return SfntStatus::invalid_input;
    if(first.glyph>sub.last_covered_glyph)return SfntStatus::ok;
    const u32 cov=sfnt_coverage_index(sub.coverage,first.glyph);if(cov==sfnt_not_covered)return SfntStatus::ok;
    SfntGposMatch result{};result.first_input=start;result.second_input=0xffffffffu;result.next_input=start+1;result.matched=true;
    if(sub.type==1){const u32 at=sub.format==1?6u:8+cov*sub.value_bytes[0];result.values[0]=value_view(sub.table,at,sub.value_formats[0],sub.value_bytes[0]);*output=result;return SfntStatus::ok;}
    u32 second=start+1;for(;second<end;++second){if(!spend(work))return SfntStatus::resource_limit;const auto& glyph=sfnt_glyph_at(glyphs,second);if(!glyph_valid(glyph,sub.glyph_limit))return SfntStatus::invalid_input;if(!sfnt_lookup_ignores(gdef,filter,glyph.glyph,glyph.glyph_class))break;}
    if(second==end)return SfntStatus::ok;const u16 glyph=sfnt_glyph_at(glyphs,second).glyph;ConstSlice<u8> parent=sub.table;u32 at=0;
    if(sub.format==1){auto s=tail(sub.table,r16(sub.table,10+cov*2),10+static_cast<u32>(sub.counts[0])*2,&parent);if(s!=SfntStatus::ok)return s;
        u32 lo=0,hi=r16(parent,0);const u32 stride=2u+sub.value_bytes[0]+sub.value_bytes[1];bool found=false;
        while(lo<hi){if(!spend(work))return SfntStatus::resource_limit;const u32 mid=lo+(hi-lo)/2;const u16 candidate=r16(parent,2+mid*stride);
            if(glyph<candidate)hi=mid;else if(glyph>candidate)lo=mid+1;else{at=4+mid*stride;found=true;break;}}
        if(!found)return SfntStatus::ok;
    } else {const u16 c1=sfnt_class_value(sub.classes[0],first.glyph),c2=sfnt_class_value(sub.classes[1],glyph);if(c1>=sub.counts[0] || c2>=sub.counts[1])return SfntStatus::ok;
        at=16+static_cast<u32>((static_cast<u64>(c1)*sub.counts[1]+c2)*(sub.value_bytes[0]+sub.value_bytes[1]));}
    result.values[0]=value_view(parent,at,sub.value_formats[0],sub.value_bytes[0]);result.values[1]=value_view(parent,at+sub.value_bytes[0],sub.value_formats[1],sub.value_bytes[1]);result.second_input=second;result.next_input=second+(sub.value_formats[1]?1u:0u);*output=result;return SfntStatus::ok;
}
}
u16 sfnt_position_value_size(u16 format) noexcept {if(format&0xff00)return 0xffff;u16 bytes=0;while(format){format=static_cast<u16>(format&(format-1));bytes+=2;}return bytes;}
SfntStatus sfnt_value_record_read(const SfntValueRecord& record,SfntPositionValue* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};SfntPositionValue result{};u32 at=record.offset;
    for(u32 i=0;i<8;++i)if(record.format&(1u<<i)){const u16 v=r16(record.parent,at);at+=2;if(i<4)result.design[i]=static_cast<i16>(v);else if(v){const auto s=sfnt_device_open({record.parent.data+v,record.parent.count-v},&result.devices[i-4]);if(s!=SfntStatus::ok)return s;}}
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_anchor_open(ConstSlice<u8> bytes,SfntAnchor* output,u32 work,u32* used) noexcept {
    const WorkReport report{work,work,used};if(!output)return SfntStatus::invalid_input;*output={};if(!bytes.data)return SfntStatus::invalid_input;if(!span(bytes,0,6))return SfntStatus::truncated;
    if(!spend(&work))return SfntStatus::resource_limit;const u16 fmt=r16(bytes,0);if(fmt<1 || fmt>3)return SfntStatus::unsupported;const u32 size=fmt==1?6u:fmt==2?8u:10u;if(!span(bytes,0,size))return SfntStatus::truncated;
    if(fmt==3)for(u32 i=0;i<2;++i){SfntDevice d{};const auto s=device(bytes,r16(bytes,6+i*2),size,&d,&work);if(s!=SfntStatus::ok)return s;}
    *output={bytes};return SfntStatus::ok;
}
SfntStatus sfnt_anchor_read(const SfntAnchor& anchor,SfntAnchorValue* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};SfntAnchorValue result{};result.format=r16(anchor.table,0);result.x=static_cast<i16>(r16(anchor.table,2));result.y=static_cast<i16>(r16(anchor.table,4));
    if(result.format==2)result.point=r16(anchor.table,6);
    if(result.format==3)for(u32 i=0;i<2;++i){const u16 at=r16(anchor.table,6+i*2);if(at){const auto s=sfnt_device_open({anchor.table.data+at,anchor.table.count-at},&result.devices[i]);if(s!=SfntStatus::ok)return s;}}
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_position_value_resolve(const SfntPositionValue& value,const SfntPositionInstance& instance,SfntPositionAdjustment* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};if(static_cast<u8>(instance.arithmetic)>1)return SfntStatus::invalid_input;SfntPositionAdjustment result{};
    for(u32 i=0;i<4;++i){result.design[i]=value.design[i];const auto& d=value.devices[i];if(!d.format)continue;
        if(d.format==0x8000){if(!instance.variable)continue;u32 index=0;sfnt_device_variation_index(d,&index);if(index==0xffffffffu)continue;if(!instance.variations)return SfntStatus::missing_table;double delta=0;const auto s=item_variation_delta(*instance.variations,index,&delta,instance.arithmetic);if(s!=SfntStatus::ok)return s;result.design[i]+=delta;}
        else {if(d.format>3)return SfntStatus::invalid_input;if(instance.variable)continue;const auto s=sfnt_device_pixels(d,(i&1)?instance.ppem_y:instance.ppem_x,&result.pixels[i]);if(s!=SfntStatus::ok)return s;}}
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_gpos_subtable_open(ConstSlice<u8> table,u16 type,u32 limit,SfntGposSubtable* output,u32 work,u32* used) noexcept {
    const WorkReport report{work,work,used};if(!output)return SfntStatus::invalid_input;return open(table,type,limit,output,&work);
}
SfntStatus sfnt_gpos_subtable_match_view(const SfntGposSubtable& sub,const SfntGdef& gdef,const SfntLookupFilter& filter,SfntGlyphView glyphs,u32 start,u32 end,SfntGposMatch* output,u32 work,u32* used) noexcept {
    const WorkReport report{work,work,used};if(!output)return SfntStatus::invalid_input;*output={};if(!sfnt_glyph_view_valid(glyphs) || start>=end || end>sfnt_glyph_count(glyphs))return SfntStatus::invalid_input;return match(sub,gdef,filter,glyphs,start,end,output,&work);
}
}
