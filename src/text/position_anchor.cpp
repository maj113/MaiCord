#include "position_anchor.hpp"
#include "maicord/text/base.hpp"
namespace maicord::position_detail {namespace {
bool spend(u64* operations,u64* work,u64 amount=1) noexcept {if(amount>*work)return false;*work-=amount;*operations+=amount;return true;}
}
SfntStatus anchor_point(const SfntAnchor& anchor,const SfntPositionInstance& instance,double* x,double* y,
                          u64* operations,u64* work) noexcept {
    if(!spend(operations,work))return SfntStatus::resource_limit;SfntAnchorValue value{};auto s=sfnt_anchor_read(anchor,&value);if(s!=SfntStatus::ok)return s;
    *x=value.x;*y=value.y;
    if(value.format==2)return SfntStatus::unsupported;
    if(instance.variable && value.format==3)for(u32 i=0;i<2;++i){u32 index=0;if(!sfnt_device_variation_index(value.devices[i],&index) || index==0xffffffffu)continue;
        if(!instance.variations)return SfntStatus::missing_table;ItemVariationData data{};s=item_variation_data(instance.variations->store,static_cast<u16>(index>>16),&data);if(s!=SfntStatus::ok)return s;
        if(!spend(operations,work,static_cast<u64>(data.region_count)+1))return SfntStatus::resource_limit;double delta=0;s=item_variation_delta(*instance.variations,index,&delta,instance.arithmetic);if(s!=SfntStatus::ok)return s;if(i)*y+=delta;else *x+=delta;}
    if(!is_finite(*x)||!is_finite(*y))return SfntStatus::resource_limit;
    if(instance.variable&&value.format==3&&instance.round_variable_anchors){
        double* values[]{x,y};for(auto* coordinate:values){const double biased=*coordinate+.5;
            if(biased<=-0x1p62||biased>=0x1p62)return SfntStatus::resource_limit;
            const auto truncated=static_cast<i64>(biased);*coordinate=static_cast<double>(truncated)-(static_cast<double>(truncated)>biased?1:0);}}
    return SfntStatus::ok;
}
SfntStatus mark_position(const SfntAnchor& base,const SfntAnchor& mark,ConstSlice<u8> header,
                            const SfntPosition& parent,SfntPosition* value,double distance,double target_nominal,double mark_nominal,bool rtl,
                            const SfntPositionInstance& instance,SfntNullAnchorPolicy policy,bool* applied,bool* compatibility,
                            u64* operations,u64* work) noexcept {
    *applied=*compatibility=false;double bx=0,by=0,mx=0,my=0;
    if(!base.table.data){if(policy==SfntNullAnchorPolicy::skip)return SfntStatus::ok;if(header.count<6)return SfntStatus::truncated;
        // A null offset points at the parent header in the Windows legacy
        // path. It is usable only when its first word looks like an anchor
        // format; larger parent counts are rejected rather than positioned.
        const u16 format=byte_load_be16(header.data);if(!format||format>3)return SfntStatus::ok;if(instance.variable)return SfntStatus::unsupported;
        bx=static_cast<i16>(byte_load_be16(header.data+2));by=static_cast<i16>(byte_load_be16(header.data+4));*compatibility=true;
    }else {const auto s=anchor_point(base,instance,&bx,&by,operations,work);if(s!=SfntStatus::ok)return s;}
    const auto s=anchor_point(mark,instance,&mx,&my,operations,work);if(s!=SfntStatus::ok)return s;
    auto candidate=*value;candidate.advance_offset=parent.advance_offset-distance+(rtl?target_nominal-mark_nominal-(bx-mx):bx-mx);
    candidate.ascender_offset=parent.ascender_offset+by-my;
    if(!is_finite(candidate.advance) || !is_finite(candidate.advance_offset) || !is_finite(candidate.ascender_offset))return SfntStatus::resource_limit;
    *value=candidate;*applied=true;return SfntStatus::ok;
}
}
