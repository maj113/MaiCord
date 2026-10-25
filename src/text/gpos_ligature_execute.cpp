#include "maicord/text/gpos_ligature_execute.hpp"
#include "maicord/core/memory.hpp"
#include "position_anchor.hpp"
namespace maicord {namespace {
bool integral(double value) noexcept {
    u64 bits=0;copy_bytes(&bits,&value,8);bits&=0x7fffffffffffffffull;if(!bits)return true;
    const u32 exponent=static_cast<u32>(bits>>52);if(exponent<1023)return false;if(exponent>=1075)return true;
    return !(bits&((u64{1}<<(1075-exponent))-1));
}
bool spend(SfntLigatureExecution* out,u64* work,u64 amount=1) noexcept {if(amount>*work)return false;*work-=amount;out->operations+=amount;return true;}
}
SfntStatus sfnt_position_execute_ligature_lookup(const SfntGposAttachmentLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,
                                                  ConstSlice<double> nominal,ConstSlice<SfntMarkAssociation> associations,ConstSlice<u32> targets,Slice<SfntPosition> positions,
                                                  u32 start,u32 end,bool rtl,const SfntPositionInstance& instance,SfntNullAnchorPolicy policy,
                                                  SfntComponentAnchorPolicy component_policy,Slice<double> prefix,SfntLigatureExecution* out,
                                                  u64 work,SfntAnchorPointResolver resolver,void* context,SfntPositionApplication application) noexcept {
    if(!out)return SfntStatus::invalid_input;reset_value(out);const u32 count=sfnt_glyph_count(glyphs);
    if(!sfnt_glyph_view_valid(glyphs) || start>end || end>count || nominal.count<count || associations.count<count || positions.count<count || (count && (!nominal.data || !associations.data || !positions.data)) || (targets.count && !targets.data) || (prefix.count && !prefix.data) || static_cast<u8>(policy)>1 || static_cast<u8>(component_policy)>1 || static_cast<u8>(instance.arithmetic)>1)return SfntStatus::invalid_input;
    if(static_cast<u8>(application)>1)return SfntStatus::invalid_input;const bool selected=application==SfntPositionApplication::selected;
    if(selected && start<end)end=start+1;
    if(!selected && prefix.count && prefix.count<targets.count)return SfntStatus::output_too_small;
    if(!lookup.subtables.count || start==end)return SfntStatus::ok;
    if(lookup.subtables.data[0].type!=5)return SfntStatus::unsupported;const u32 limit=lookup.subtables.data[0].glyph_limit;
    for(u32 i=0;!selected && i<targets.count;++i){if(!spend(out,&work))return SfntStatus::resource_limit;if(targets.data[i]>=count || (i && targets.data[i]<=targets.data[i-1]))return SfntStatus::invalid_input;}
    bool exact=!selected && prefix.count!=0;double magnitude=0,sum=0,start_sum=0;u32 target_at=0;
    for(u32 at=0;!selected && at<count;++at){if(!spend(out,&work))return SfntStatus::resource_limit;++out->scan_steps;
        const auto& glyph=sfnt_glyph_at(glyphs,at);const auto& association=associations.data[at];const double advance=positions.data[at].advance;
        if(glyph.glyph>=limit)return SfntStatus::glyph_out_of_range;
        if(static_cast<u8>(glyph.glyph_class)>4 || (association.target!=0xffffffffu && (association.target>=targets.count || targets.data[association.target]>=at)) || !is_finite(nominal.data[at]) || !is_finite(advance))return SfntStatus::invalid_input;
        if(exact){if(at==start)start_sum=sum;if(target_at<targets.count && targets.data[target_at]==at)prefix.data[target_at]=sum;
            magnitude+=advance<0?-advance:advance;exact=integral(advance) && magnitude<=4503599627370496.0;if(exact)sum+=advance;}
        if(target_at<targets.count && targets.data[target_at]==at)++target_at;
    }
    u32 previous_target=0xffffffffu,previous_at=0;double distance=0;
    double current_sum=start_sum;
    for(u32 at=start;at<end;++at){if(!spend(out,&work))return SfntStatus::resource_limit;++out->scan_steps;const double at_sum=current_sum;if(exact)current_sum+=positions.data[at].advance;
        const auto& glyph=sfnt_glyph_at(glyphs,at);const auto& association=associations.data[at];
        if(selected){if(glyph.glyph>=limit)return SfntStatus::glyph_out_of_range;if(static_cast<u8>(glyph.glyph_class)>4 || !is_finite(nominal.data[at]) || !is_finite(positions.data[at].advance) || (association.target!=0xffffffffu && (association.target>=targets.count || targets.data[association.target]>=at)))return SfntStatus::invalid_input;}
        if(glyph.glyph_class!=SfntGlyphClass::mark || association.target==0xffffffffu || (!selected && sfnt_lookup_ignores(gdef,lookup.filter,glyph.glyph,glyph.glyph_class)))continue;
        const u32 target=targets.data[association.target];
        if(selected){if(!spend(out,&work))return SfntStatus::resource_limit;const auto& parent=sfnt_glyph_at(glyphs,target);if(parent.glyph>=limit)return SfntStatus::glyph_out_of_range;if(static_cast<u8>(parent.glyph_class)>4 || !is_finite(nominal.data[target]))return SfntStatus::invalid_input;}
        if(exact){distance=at_sum-prefix.data[association.target];++out->prefix_queries;}
        else {u32 from=target;if(previous_target==target)from=previous_at;else distance=0;
            for(u32 i=from;i<at;++i){if(!spend(out,&work))return SfntStatus::resource_limit;++out->ordered_steps;distance+=positions.data[i].advance;if(!is_finite(distance))return SfntStatus::resource_limit;}
            previous_target=target;previous_at=at;}
        for(u32 i=0;i<lookup.subtables.count;++i){if(!spend(out,&work))return SfntStatus::resource_limit;const auto& sub=lookup.subtables.data[i];
            SfntGposMarkAnchor mark{};auto s=sfnt_gpos_mark_anchor(sub,glyph.glyph,&mark);if(s!=SfntStatus::ok)return s;if(!mark.covered)continue;
            const auto& parent_glyph=sfnt_glyph_at(glyphs,target);SfntGposTargetAnchors anchors{};s=sfnt_gpos_target_anchors(sub,parent_glyph.glyph,&anchors);if(s!=SfntStatus::ok)return s;if(!anchors.covered)continue;
            const bool unknown=association.component==sfnt_component_unknown;const u32 component=unknown?0:association.component;if(component>=anchors.component_count){if(component_policy==SfntComponentAnchorPolicy::strict)return SfntStatus::invalid_input;++out->missing_components;continue;}
            SfntAnchor base{};s=sfnt_gpos_target_anchor(anchors,static_cast<u16>(component),mark.mark_class,&base);if(s!=SfntStatus::ok)return s;
            bool applied=false,compatibility=false;s=position_detail::mark_position(base,mark.anchor,anchors.parent,parent_glyph.glyph,glyph.glyph,positions.data[target],&positions.data[at],distance,nominal.data[target],nominal.data[at],rtl,instance,unknown?policy:SfntNullAnchorPolicy::skip,&applied,&compatibility,&out->operations,&work,resolver,context);
            if(s!=SfntStatus::ok)return s;if(!applied)continue;++out->matches;if(compatibility)++out->compatibility_anchors;break;
        }
    }return SfntStatus::ok;
}
}
