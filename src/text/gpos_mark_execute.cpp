#include "maicord/text/gpos_mark_execute.hpp"
#include "maicord/core/memory.hpp"
#include "position_anchor.hpp"
#include "maicord/text/components.hpp"
namespace maicord {namespace {
bool spend(SfntMarkExecution* out,u64* work,u64 amount=1) noexcept {if(amount>*work)return false;*work-=amount;out->operations+=amount;return true;}
bool eligible(const SfntGsubGlyph& glyph,u8 type,const SfntGdef& gdef,const SfntLookupFilter& filter) noexcept {
    return type==4?glyph.glyph_class!=SfntGlyphClass::mark:!sfnt_lookup_ignores(gdef,filter,glyph.glyph,glyph.glyph_class);
}
SfntStatus glyph_check(const SfntGsubGlyph& glyph,u32 limit) noexcept {
    if(glyph.glyph>=limit)return SfntStatus::glyph_out_of_range;return static_cast<u8>(glyph.glyph_class)<=4?SfntStatus::ok:SfntStatus::invalid_input;
}
}
SfntStatus sfnt_position_execute_mark_lookup(const SfntGposAttachmentLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,ConstSlice<double> nominal,ConstSlice<u8> flags,Slice<SfntPosition> positions,u32 start,u32 end,bool rtl,const SfntPositionInstance& instance,SfntNullAnchorPolicy policy,SfntMarkExecution* out,u64 work,SfntAnchorPointResolver resolver,void* context,ConstSlice<SfntMarkAssociation> associations,SfntPositionApplication application,bool component_records,ConstSlice<u32> directory) noexcept {
    if(!out)return SfntStatus::invalid_input;reset_value(out);const u32 count=sfnt_glyph_count(glyphs);
    if(!sfnt_glyph_view_valid(glyphs) || start>end || end>count || nominal.count<count || flags.count<count || positions.count<count || (count && (!nominal.data || !flags.data || !positions.data)) || static_cast<u8>(policy)>1 || static_cast<u8>(instance.arithmetic)>1)return SfntStatus::invalid_input;
    if(static_cast<u8>(application)>1 || (associations.count && (associations.count<count || !associations.data)) || (directory.count&&!directory.data))return SfntStatus::invalid_input;
    const bool selected=application==SfntPositionApplication::selected;if(selected && start<end)end=start+1;
    if(!lookup.subtables.count || start==end)return SfntStatus::ok;const u8 type=lookup.subtables.data[0].type;const u32 limit=lookup.subtables.data[0].glyph_limit;
    if(type!=4 && type!=6)return SfntStatus::unsupported;
    if(component_records&&glyphs.record_size!=sizeof(SfntComponentGlyph))return SfntStatus::invalid_input;
    const auto target_eligible=[&](const SfntGsubGlyph& glyph) noexcept {return eligible(glyph,type,gdef,lookup.filter)&&
        (type!=4||!component_records||reinterpret_cast<const SfntComponentGlyph*>(&glyph)->component!=0xffffffffu);};
    u32 target=0xffffffffu;double distance=0;
    // A glyph newly classified as a mark can still start a source cluster.
    // Windows attaches it to the preceding eligible mark across that boundary.
    for(u32 before=type==6 && sfnt_glyph_at(glyphs,start).glyph_class!=SfntGlyphClass::mark && (flags.data[start]&sfnt_position_cluster_start)?0:start;before;){if(!spend(out,&work))return SfntStatus::resource_limit;++out->search_steps;--before;
        const auto& glyph=sfnt_glyph_at(glyphs,before);auto s=glyph_check(glyph,limit);if(s!=SfntStatus::ok)return s;
        distance+=positions.data[before].advance;if(!is_finite(distance))return SfntStatus::resource_limit;
        if(target_eligible(glyph)){target=before;break;}if(type==6 && glyph.glyph_class!=SfntGlyphClass::mark && (flags.data[before]&sfnt_position_cluster_start))break;}
    for(u32 at=start;at<end;++at){if(!spend(out,&work))return SfntStatus::resource_limit;++out->search_steps;
        const auto& glyph=sfnt_glyph_at(glyphs,at);auto s=glyph_check(glyph,limit);if(s!=SfntStatus::ok)return s;
        if(type==6 && glyph.glyph_class!=SfntGlyphClass::mark && (flags.data[at]&sfnt_position_cluster_start)){target=0xffffffffu;distance=0;}
        u32 attached_target=target;double attached_distance=distance;
        if(type==4&&instance.source_mark_targets&&(glyph.reserved&sfnt_mark_source_target)&&glyph.glyph_class==SfntGlyphClass::mark){
            if(!associations.count)return SfntStatus::invalid_input;const u32 slot=associations[at].target;
            if(slot!=0xffffffffu){if(slot>=directory.count)return SfntStatus::invalid_input;attached_target=directory[slot];
                if(attached_target>=at)return SfntStatus::invalid_input;const auto& owner=sfnt_glyph_at(glyphs,attached_target);
                s=glyph_check(owner,limit);if(s!=SfntStatus::ok)return s;if(!target_eligible(owner))return SfntStatus::invalid_input;
                if(attached_target!=target){attached_distance=0;
                    for(u32 i=attached_target;i<at;++i){if(!spend(out,&work))return SfntStatus::resource_limit;++out->search_steps;attached_distance+=positions[i].advance;}
                    if(!is_finite(attached_distance))return SfntStatus::resource_limit;}}
        }
        if(glyph.glyph_class==SfntGlyphClass::mark && (selected || !sfnt_lookup_ignores(gdef,lookup.filter,glyph.glyph,glyph.glyph_class)) && target!=0xffffffffu && (type==4 || sfnt_glyph_at(glyphs,target).glyph_class==SfntGlyphClass::mark)){
            // Only two resolved owners can establish a component conflict.
            // GSUB can create a mark whose predecessor sequence has no owner.
            const bool same_component=!associations.count || type!=6 || associations.data[at].target==0xffffffffu || associations.data[target].target==0xffffffffu || (associations.data[at].target==associations.data[target].target && associations.data[at].component==associations.data[target].component);
            for(u32 i=0;same_component && i<lookup.subtables.count;++i){if(!spend(out,&work))return SfntStatus::resource_limit;const auto& sub=lookup.subtables.data[i];
                SfntGposMarkAnchor mark{};s=sfnt_gpos_mark_anchor(sub,glyph.glyph,&mark);if(s!=SfntStatus::ok)return s;if(!mark.covered)continue;
                const auto& target_glyph=sfnt_glyph_at(glyphs,attached_target);SfntGposTargetAnchors targets{};s=sfnt_gpos_target_anchors(sub,target_glyph.glyph,&targets);if(s!=SfntStatus::ok)return s;if(!targets.covered)continue;
                SfntAnchor base{};s=sfnt_gpos_target_anchor(targets,0,mark.mark_class,&base);if(s!=SfntStatus::ok)return s;
                bool applied=false,compatibility=false;
                s=position_detail::mark_position(base,mark.anchor,targets.parent,target_glyph.glyph,glyph.glyph,positions.data[attached_target],&positions.data[at],attached_distance,nominal.data[attached_target],nominal.data[at],rtl,instance,policy,&applied,&compatibility,&out->operations,&work,resolver,context);
                if(s!=SfntStatus::ok)return s;if(!applied)continue;++out->matches;if(compatibility)++out->compatibility_anchors;break;
            }
        }
        if(target_eligible(glyph)){target=at;distance=positions.data[at].advance;}
        else distance+=positions.data[at].advance;
        if(!is_finite(distance))return SfntStatus::resource_limit;
    }return SfntStatus::ok;
}
}
