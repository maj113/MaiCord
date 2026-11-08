#include "position_anchor.hpp"
namespace maicord::position_detail {namespace {
inline bool spend(u64* operations,u64* work,u64 amount=1) noexcept {if(amount>*work)return false;*work-=amount;*operations+=amount;return true;}
}
SfntStatus anchor_point(const SfntAnchor& anchor,const SfntPositionInstance& instance,double* x,double* y,
                          u64* operations,u64* work) noexcept {
    if(!spend(operations,work))return SfntStatus::resource_limit;SfntAnchorValue value{};auto s=sfnt_anchor_read(anchor,&value);if(s!=SfntStatus::ok)return s;
    // Format 2's contour point only refines a hinted outline; unhinted layout
    // uses its design coordinates (OpenType GPOS Anchor Table Format 2; HarfBuzz).
    *x=value.x;*y=value.y;
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
SfntStatus mark_position(const SfntAnchor& base,const SfntAnchor& mark,
                            const SfntPosition& parent,SfntPosition* value,double distance,bool rtl,
                            const SfntPositionInstance& instance,bool* applied,u64* operations,u64* work) noexcept {
    *applied=false;double bx=0,by=0,mx=0,my=0;
    // A NULL target anchor offset means this mark class does not attach to the
    // target (OpenType GPOS lookup types 4-6); the subtable does not apply.
    if(!base.table.data)return SfntStatus::ok;
    auto s=anchor_point(base,instance,&bx,&by,operations,work);if(s!=SfntStatus::ok)return s;
    s=anchor_point(mark,instance,&mx,&my,operations,work);if(s!=SfntStatus::ok)return s;
    // Right to left a glyph is drawn left of its pen by its advance plus offset, so the mark's own (shaped) advance
    // enters; a zero-advance mark keeps none (HarfBuzz zeroes mark advances before resolving attachments).
    auto candidate=*value;candidate.advance_offset=parent.advance_offset-distance+(rtl?parent.advance-value->advance-(bx-mx):bx-mx);
    candidate.ascender_offset=parent.ascender_offset+by-my;
    if(!is_finite(candidate.advance) || !is_finite(candidate.advance_offset) || !is_finite(candidate.ascender_offset))return SfntStatus::resource_limit;
    *value=candidate;*applied=true;return SfntStatus::ok;
}
}
