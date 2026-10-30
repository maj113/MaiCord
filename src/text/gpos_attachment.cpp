#include "maicord/text/gpos_attachment.hpp"
#include "maicord/core/memory.hpp"
#include "layout_read.hpp"
#include "maicord/text/base.hpp"

namespace maicord {namespace {
using layout_read::WorkReport;using layout_read::r16;using layout_read::tail;
bool span(const ConstSlice<u8>& b,u32 at,u64 count) noexcept {return at<=b.count && count<=b.count-at && (!count || b.data);}
inline ConstSlice<u8> slice(const ConstSlice<u8>& b,u32 at) noexcept {return {b.data+at,b.count-at};}
SfntStatus cover(ConstSlice<u8> parent,u16 at,u32 minimum,u32 limit,u32 count,SfntCoverage* out,u32* work) noexcept {
    ConstSlice<u8> bytes{};auto s=tail(parent,at,minimum,&bytes);if(s!=SfntStatus::ok)return s;if(!span(bytes,0,4))return SfntStatus::truncated;
    if(!spend(work,r16(bytes,2)))return SfntStatus::resource_limit;s=sfnt_coverage_open(bytes,limit,out);if(s!=SfntStatus::ok)return s;return out->covered_count==count?SfntStatus::ok:SfntStatus::malformed;
}
SfntStatus anchor(ConstSlice<u8> parent,u16 at,u32 minimum,bool nullable,u32* work) noexcept {
    if(!spend(work))return SfntStatus::resource_limit;if(!at)return nullable?SfntStatus::ok:SfntStatus::malformed;
    ConstSlice<u8> bytes{};auto s=tail(parent,at,minimum,&bytes);if(s!=SfntStatus::ok)return s;SfntAnchor a{};u32 used=0;s=sfnt_anchor_open(bytes,&a,*work,&used);*work-=used;return s;
}
inline SfntAnchor anchor_view(const ConstSlice<u8>& parent,u32 field) noexcept {const u16 at=r16(parent,field);return at?SfntAnchor{slice(parent,at)}:SfntAnchor{};}
SfntStatus matrix(ConstSlice<u8> parent,u32 rows,u32 classes,u32* work) noexcept {
    const u64 count=static_cast<u64>(rows)*classes,end=2+2*count;if(!span(parent,0,end))return SfntStatus::truncated;
    return spend(work,static_cast<u32>(count))?SfntStatus::ok:SfntStatus::resource_limit;
}
SfntStatus open(ConstSlice<u8> bytes,u16 type,u32 limit,SfntGposAttachment* out,u32* work) noexcept {
    reset_value(out);if(!bytes.data || !limit || limit>65536)return SfntStatus::invalid_input;if(type<3 || type>6)return SfntStatus::unsupported;
    if(!spend(work))return SfntStatus::resource_limit;if(!span(bytes,0,type==3?6u:12u))return SfntStatus::truncated;if(r16(bytes,0)!=1)return SfntStatus::unsupported;
    SfntGposAttachment result{};result.glyph_limit=limit;result.type=static_cast<u8>(type);SfntStatus s;
    if(type==3){const u32 count=r16(bytes,4),end=6+count*4;if(!span(bytes,0,end))return SfntStatus::truncated;result.counts[0]=static_cast<u16>(count);result.arrays[0]=bytes;
        s=cover(bytes,r16(bytes,2),end,limit,count,&result.coverage[0],work);if(s!=SfntStatus::ok)return s;
        for(u32 i=0;i<count*2;++i){s=anchor(bytes,r16(bytes,6+i*2),end,true,work);if(s!=SfntStatus::ok)return s;}
    }else {result.class_count=r16(bytes,6);
        for(u32 i=0;i<2;++i){s=tail(bytes,r16(bytes,8+i*2),12,&result.arrays[i]);if(s!=SfntStatus::ok)return s;result.counts[i]=r16(result.arrays[i],0);
            s=cover(bytes,r16(bytes,2+i*2),12,limit,result.counts[i],&result.coverage[i],work);if(s!=SfntStatus::ok)return s;}
        const auto marks=result.arrays[0];const u32 count=result.counts[0],end=2+count*4;if(!span(marks,0,end))return SfntStatus::truncated;
        for(u32 i=0;i<count;++i){if(!spend(work))return SfntStatus::resource_limit;if(r16(marks,2+i*4)>=result.class_count)return SfntStatus::malformed;
            s=anchor(marks,r16(marks,4+i*4),end,false,work);if(s!=SfntStatus::ok)return s;}
        const auto targets=result.arrays[1];if(type!=5){s=matrix(targets,result.counts[1],result.class_count,work);if(s!=SfntStatus::ok)return s;}
        else {const u32 header=2+static_cast<u32>(result.counts[1])*2;if(!span(targets,0,header))return SfntStatus::truncated;
            for(u32 i=0;i<result.counts[1];++i){if(!spend(work))return SfntStatus::resource_limit;ConstSlice<u8> lig{};s=tail(targets,r16(targets,2+i*2),header,&lig);if(s!=SfntStatus::ok)return s;
                s=matrix(lig,r16(lig,0),result.class_count,work);if(s!=SfntStatus::ok)return s;}}
    }
    *out=result;return SfntStatus::ok;
}
}
SfntStatus sfnt_gpos_attachment_open(ConstSlice<u8> bytes,u16 type,u32 limit,SfntGposAttachment* output,u32 work,u32* used) noexcept {
    const WorkReport report{work,work,used};if(!output)return SfntStatus::invalid_input;return open(bytes,type,limit,output,&work);
}
SfntStatus sfnt_gpos_cursive_anchors(const SfntGposAttachment& sub,u16 glyph,SfntGposCursiveAnchors* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(sub.type!=3)return SfntStatus::invalid_input;if(glyph>=sub.glyph_limit)return SfntStatus::glyph_out_of_range;
    const u32 index=sfnt_coverage_index(sub.coverage[0],glyph);if(index==sfnt_not_covered)return SfntStatus::ok;
    *output={anchor_view(sub.arrays[0],6+index*4),anchor_view(sub.arrays[0],8+index*4),true};return SfntStatus::ok;
}
SfntStatus sfnt_gpos_mark_anchor(const SfntGposAttachment& sub,u16 glyph,SfntGposMarkAnchor* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(sub.type<4 || sub.type>6)return SfntStatus::invalid_input;if(glyph>=sub.glyph_limit)return SfntStatus::glyph_out_of_range;
    const u32 index=sfnt_coverage_index(sub.coverage[0],glyph);if(index==sfnt_not_covered)return SfntStatus::ok;
    *output={anchor_view(sub.arrays[0],4+index*4),r16(sub.arrays[0],2+index*4),true};return SfntStatus::ok;
}
SfntStatus sfnt_gpos_target_anchors(const SfntGposAttachment& sub,u16 glyph,SfntGposTargetAnchors* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(sub.type<4 || sub.type>6)return SfntStatus::invalid_input;if(glyph>=sub.glyph_limit)return SfntStatus::glyph_out_of_range;
    const u32 index=sfnt_coverage_index(sub.coverage[1],glyph);if(index==sfnt_not_covered)return SfntStatus::ok;
    SfntGposTargetAnchors result{};result.parent=sub.arrays[1];result.class_count=sub.class_count;result.covered=true;
    if(sub.type==5){result.parent=slice(result.parent,r16(result.parent,2+index*2));result.offset=2;result.component_count=r16(result.parent,0);}
    else {result.offset=2+index*sub.class_count*2;result.component_count=1;}
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_gpos_target_anchor(const SfntGposTargetAnchors& target,u16 component,u16 mark_class,SfntAnchor* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!target.covered || component>=target.component_count || mark_class>=target.class_count)return SfntStatus::invalid_input;
    const u16 at=r16(target.parent,target.offset+(static_cast<u32>(component)*target.class_count+mark_class)*2);
    if(!at)return SfntStatus::ok;
    const u32 minimum=2+static_cast<u32>(r16(target.parent,0))*target.class_count*2;
    ConstSlice<u8> bytes{};const auto status=tail(target.parent,at,minimum,&bytes);
    return status==SfntStatus::ok?sfnt_anchor_open(bytes,output,3):status;
}
}
