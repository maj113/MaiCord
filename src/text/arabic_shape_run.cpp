#include "maicord/text/arabic_shape_run.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/arabic_marks.hpp"
#include "maicord/text/arabic_joining.hpp"

namespace maicord {namespace {
template<class T> void reverse_prefix(T* values,u32 first,u32 end) noexcept {
    while(first<end){--end;if(first==end)break;const auto value=values[first];values[first++]=values[end];values[end]=value;}
}
bool join_control(u32 scalar) noexcept {return scalar==0x200c || scalar==0x200d;}
struct ControlPolicy {const SfntGdef* gdef;const u8* sources;u32 units;u16 space;bool ltr=false,cgj=false;SfntComponentForest* forest=nullptr;};
constexpr u8 cgj_marker=128;
constexpr u8 ltr_control_marker=64;
constexpr u8 initial_join_marker=32;
SfntStatus prepare_controls(void* opaque,SfntGlyphBuffer* buffer,SfntClusterMap* clusters,SfntComponentForest* forest,const SfntGsubExecutionScratch& scratch,u64 work) noexcept {
    auto& policy=*static_cast<ControlPolicy*>(opaque);policy.forest=forest;
    const auto initial=sfnt_glyph_buffer_view(*buffer);const u32 count=sfnt_glyph_count(initial);
    if(count>work)return SfntStatus::resource_limit;work-=count;
    for(u32 i=0;i<count;++i){auto& g=*reinterpret_cast<SfntComponentGlyph*>(const_cast<SfntGsubGlyph*>(&sfnt_glyph_at(initial,i)));
        if(g.component>=forest->nodes.count)return SfntStatus::invalid_input;
        if(g.glyph.reserved&initial_join_marker){g.glyph.reserved&=~initial_join_marker;auto& metadata=forest->nodes[g.component].reserved;metadata=static_cast<u16>((metadata&~initial_join_marker)|512u);}
        if(g.glyph.reserved&cgj_marker){g.glyph.reserved&=~cgj_marker;auto& node=forest->nodes[g.component];node.reserved=static_cast<u16>((node.reserved&~128u)|256u);}
    }
    if(policy.ltr&&!policy.cgj)return SfntStatus::ok;
    for(u32 i=1;i<sfnt_glyph_count(sfnt_glyph_buffer_view(*buffer));){
        if(!work)return SfntStatus::resource_limit;--work;const auto view=sfnt_glyph_buffer_view(*buffer);
        const auto& g=*reinterpret_cast<const SfntComponentGlyph*>(&sfnt_glyph_at(view,i));
        // Before GSUB each uncomposed control still owns its initial node.
        if(g.component>=forest->nodes.count)return SfntStatus::invalid_input;
        if(!(forest->nodes[g.component].reserved&256u)){++i;continue;}
        SfntComponentGlyph replacement=*reinterpret_cast<const SfntComponentGlyph*>(&sfnt_glyph_at(view,i-1));
        SfntClusterMerge merge{};u64 used=0;auto status=sfnt_cluster_map_plan(*clusters,minimum(replacement.source,g.source),maximum(replacement.source,g.source),&merge,work,&used);
        if(status!=SfntStatus::ok)return status;if(used>work||merge.commit_work>work-used)return SfntStatus::resource_limit;work-=used+merge.commit_work;
        const u32 consumed[]{i-1,i};SfntComponentEdit components{};
        status=sfnt_components_edit(forest,view,{consumed,2},true,{reinterpret_cast<u8*>(&replacement),sizeof(replacement)},scratch.component_journal,&components,work);
        if(status!=SfntStatus::ok)return status;work-=components.operations;
        SfntGlyphEdit edit{};status=sfnt_glyph_buffer_replace(buffer,i-1,{consumed,2},{reinterpret_cast<const u8*>(&replacement),sizeof(replacement)},&edit,work);
        if(status!=SfntStatus::ok){sfnt_components_rollback(forest,{scratch.component_journal.data,components.undo_count});return status;}
        work-=edit.moved_records+2;sfnt_cluster_map_commit(clusters,merge);
    }
    return SfntStatus::ok;
}
SfntStatus finish_controls(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    const auto& policy=*static_cast<const ControlPolicy*>(opaque);
    if(zero.count<glyphs.count)return SfntStatus::output_too_small;if(glyphs.count>work)return SfntStatus::resource_limit;work-=glyphs.count;
    for(u32 i=0;i<glyphs.count;++i)if(glyphs[i].source>=policy.units)return SfntStatus::invalid_input;
    // GSUB2 outputs beyond the owned component sequence repeat the last
    // component's control role, just as they repeat its source and form.
    u8 previous_metadata=0;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];const u8 source=policy.ltr?0:policy.sources[g.source]&3u;bool control=source==1;
        if(policy.ltr){if(!policy.forest)return SfntStatus::invalid_input;u64 used=0;
            if(g.component!=0xffffffffu){const auto status=sfnt_component_first_metadata(*policy.forest,g.component,&previous_metadata,work,&used);if(status!=SfntStatus::ok)return status;work-=used;}control=(previous_metadata&2)!=0;}
        if(source==2){u8 metadata=1;
            if(g.component!=0xffffffffu){if(!policy.forest)return SfntStatus::invalid_input;u64 used=0;const auto status=sfnt_component_first_metadata(*policy.forest,g.component,&metadata,work,&used);if(status!=SfntStatus::ok)return status;work-=used;}
            control=(metadata&1)!=0;
        }
        if(control){g.glyph.glyph=policy.space;if(policy.ltr)g.glyph.reserved|=ltr_control_marker;}
        g.glyph.glyph_class=sfnt_gdef_glyph_class(*policy.gdef,g.glyph.glyph);
        zero[i]=static_cast<u8>(control || g.glyph.glyph_class==SfntGlyphClass::mark);
    }
    return SfntStatus::ok;
}
SfntStatus restore_cgj_clusters(void* opaque,SfntClusterMap* clusters,const SfntComponentForest&,Slice<SfntComponentGlyph>,u64 work) noexcept {
    const auto& policy=*static_cast<const ControlPolicy*>(opaque);
    // GSUB decomposition may restore component boundaries. A CGJ consumed
    // during preparation remains in its preceding source cluster nevertheless.
    if(policy.units>work)return SfntStatus::resource_limit;work-=policy.units;
    for(u32 i=1;i<policy.units;){if(!(policy.sources[i]&4u)){++i;continue;}
        const u32 first=i-1;while(i+1<policy.units&&(policy.sources[i+1]&4u))++i;
        SfntClusterMerge merge{};u64 used=0;const auto status=sfnt_cluster_map_plan(*clusters,first,i,&merge,work,&used);
        if(status!=SfntStatus::ok)return status;work-=used;if(merge.commit_work>work)return SfntStatus::resource_limit;
        work-=merge.commit_work;sfnt_cluster_map_commit(clusters,merge);++i;
    }
    return SfntStatus::ok;
}
}
SfntStatus sfnt_arabic_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntClusterGlyph> input,ConstSlice<u32> scalars,
    u32 units,ConstSlice<u32> boundaries,bool rtl,float em,const SfntShapeStorage& storage,
    SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(scalars.count!=input.count)return SfntStatus::invalid_input;
    auto status=sfnt_arabic_mark_order(scalars,storage.character_glyphs,limits.stage_work,rtl);
    if(status!=SfntStatus::ok)return status;
    if((input.count && !input.data) || (boundaries.count && !boundaries.data))return SfntStatus::invalid_input;
    if(units>limits.stage_work || static_cast<u64>(input.count)+boundaries.count>limits.stage_work)return SfntStatus::resource_limit;
    for(u32 i=0;i<input.count;++i)if(input[i].source>=units || (i && input[i].source<input[i-1].source))return SfntStatus::invalid_input;
    for(u32 i=0;i<boundaries.count;++i)if(boundaries[i]>=units || (!i && boundaries[i]) || (i && boundaries[i]<=boundaries[i-1]))return SfntStatus::invalid_input;
    bool controls=false,cgj=false,neutral=true;
    for(u32 i=0;i<scalars.count;++i){const u32 scalar=scalars[i];const bool join=join_control(scalar);
        controls|=join;cgj|=scalar==0x34f;neutral=neutral&&(scalar==32||scalar==0x34f||join);}
    controls|=cgj;
    // A run containing only spaces and these invisible controls retains logical
    // order in Windows, including CGJ consumption and control ownership.
    const bool logical=rtl||neutral;
    u32 leading=0;while(leading<scalars.count && (sfnt_arabic_combining_mark(scalars[leading])||((leading||!logical)&&scalars[leading]==0x34f)))++leading;
    const auto boundary=[&](u32 i) noexcept {return i<leading || (!sfnt_arabic_combining_mark(scalars[i])&&(logical||scalars[i]!=0x34f));};
    ControlPolicy policy{};SfntShapeGlyphPolicy glyph_policy{};
    if(controls) {
        auto* sources=reinterpret_cast<u8*>(storage.advances.data);
        if(logical||cgj){if(static_cast<u64>(storage.advances.count)*sizeof(double)<units)return SfntStatus::output_too_small;
            if(units&&!sources)return SfntStatus::invalid_input;clear_bytes(sources,units);
            for(u32 i=0;i<input.count;++i)if(join_control(scalars[i]))sources[input[i].source]=1;else if(scalars[i]==0x34f)sources[input[i].source]=static_cast<u8>(2u|(i&&i>=leading?4u:0u));}
        policy={&plan.gdef,sources,units,0};status=sfnt_cmap_glyph(cmap,32,0,&policy.space);if(status!=SfntStatus::ok)return status;
        policy.ltr=!logical;policy.cgj=cgj;
        glyph_policy={&policy,finish_controls,cgj?restore_cgj_clusters:nullptr};
    }
    if(boundaries.count) {
        u32 needed=boundaries.count,at=0,previous=0xffffffffu;
        for(u32 i=0;i<input.count;++i)if(boundary(i)){const u32 source=input[i].source;while(at<boundaries.count && boundaries[at]<source)++at;if((at==boundaries.count || boundaries[at]!=source) && previous!=source)++needed;previous=source;}
        if(needed>storage.targets.count)return SfntStatus::output_too_small;if(needed && !storage.targets.data)return SfntStatus::invalid_input;
        // Backward merge also permits boundaries to borrow the exact target
        // prefix: unread original boundaries cannot be overwritten.
        u32 b=boundaries.count,i=input.count,written=needed,last=0xffffffffu;
        while(b || i) {
            while(i && !boundary(i-1))--i;
            if(!b && !i)break;
            const u32 a=b?boundaries[b-1]:0,c=i?input[i-1].source:0;
            const u32 next=!b?c:!i?a:maximum(a,c);
            if(next!=last)storage.targets[--written]=next;last=next;
            if(b && a==next)--b;if(i && c==next)--i;
        }
        boundaries={storage.targets.data,needed};
    }
    ConstSlice<u32> order{storage.character_glyphs.data,scalars.count};
    const bool leading_cgj=scalars.count&&scalars[0]==0x34f;
    if(leading_cgj&&logical){
        if(storage.flags.count<scalars.count)return SfntStatus::output_too_small;if(!storage.flags.data)return SfntStatus::invalid_input;
        // A lone initial CGJ leaves the following joining sequence nominal until
        // a nonjoining scalar (including a transparent mark) restarts form analysis.
        u32 restart=1;while(restart<scalars.count){const auto type=unicode_properties(scalars[restart])->joining_type();
            if(type!=UnicodeJoiningType::d&&type!=UnicodeJoiningType::r&&type!=UnicodeJoiningType::l&&scalars[restart]!=0x200d)break;++restart;}
        for(u32 i=0;i<restart;++i)storage.flags[i]=0;
        const auto joining=arabic_joining_forms({scalars.data+restart,scalars.count-restart},{storage.flags.data+restart,scalars.count-restart},1,limits.stage_work);
        if(joining!=ArabicJoiningStatus::ok)return joining==ArabicJoiningStatus::work_limit?SfntStatus::resource_limit:SfntStatus::invalid_input;
    }
    if(!logical){
        const auto joining=arabic_joining_forms(scalars,storage.flags,1,limits.stage_work,true);
        if(joining!=ArabicJoiningStatus::ok)return joining==ArabicJoiningStatus::work_limit?SfntStatus::resource_limit:joining==ArabicJoiningStatus::output_too_small?SfntStatus::output_too_small:SfntStatus::invalid_input;
    }
    if(leading||cgj||(!logical&&input.count)) {
        const u64 count=static_cast<u64>(input.count)+leading;
        if(count>limits.stage_work)return SfntStatus::resource_limit;
        if(count>storage.glyphs.count || count>storage.components.count || count*sizeof(SfntClusterGlyph)>storage.substitutions.replacements.count)return SfntStatus::output_too_small;
        if(!storage.substitutions.replacements.data || reinterpret_cast<usize>(storage.substitutions.replacements.data)%alignof(SfntClusterGlyph))return SfntStatus::invalid_input;
        u16 circle=0;if(leading){status=sfnt_cmap_glyph(cmap,0x25cc,0,&circle);if(status!=SfntStatus::ok)return status;}
        const SfntGsubGlyph dotted{circle,sfnt_gdef_glyph_class(plan.gdef,circle),0};
        auto* expanded=reinterpret_cast<SfntClusterGlyph*>(storage.substitutions.replacements.data);u32 written=0;
        for(u32 i=0;i<input.count;++i){auto glyph=input[order[i]];if(leading_cgj||!logical)glyph.glyph.reserved=storage.flags[order[i]];if(scalars[order[i]]==0x34f)glyph.glyph.reserved|=cgj_marker;
            if(!logical&&(join_control(scalars[order[i]])||scalars[order[i]]==0x34f))glyph.glyph.reserved|=initial_join_marker;
            if(i<leading){glyph.source=input[i].source;expanded[written++]={dotted,glyph.source};}
            expanded[written++]=glyph;
        }
        input={expanded,static_cast<u32>(count)};reset_value(&order);
        if(!logical){
            if(count*2+boundaries.count>limits.stage_work)return SfntStatus::resource_limit;
            u32 boundary_index=0;
            for(u32 first=0;first<input.count;){u32 end=first+1;
                if(boundaries.count){while(boundary_index+1<boundaries.count&&boundaries[boundary_index+1]<=expanded[first].source)++boundary_index;
                    u32 limit=boundary_index+1<boundaries.count?boundaries[boundary_index+1]:units;
                    while(end<input.count){
                        if(expanded[end].source<limit){++end;continue;}
                        // Preparation consumes CGJ with its logical predecessor,
                        // even when the caller supplied a boundary at CGJ.
                        if(!(expanded[end].glyph.reserved&cgj_marker))break;
                        while(boundary_index+1<boundaries.count&&boundaries[boundary_index+1]<=expanded[end].source)++boundary_index;
                        limit=boundary_index+1<boundaries.count?boundaries[boundary_index+1]:units;++end;
                    }
                }else while(end<input.count&&(expanded[end].source==expanded[first].source||(expanded[end].glyph.reserved&cgj_marker)))++end;
                reverse_prefix(expanded,first,end);first=end;
            }
            reverse_prefix(expanded,0,input.count);
            // Control ownership is indexed in the opposite glyph order (including
            // inserted dotted circles and within-cluster marks); carried in component metadata.
            if(controls)for(u32 a=0,b=input.count;a<b;++a){--b;const u8 change=(expanded[a].glyph.reserved^expanded[b].glyph.reserved)&initial_join_marker;
                expanded[a].glyph.reserved^=change;expanded[b].glyph.reserved^=change;}
        }
    }
    const SfntShapeInputPolicy initial{&policy,prepare_controls};
    // Forced-LTR reorders and transforms raw positions below. Export the source
    // map and scale placements once, after that transformation.
    status=sfnt_shape_run(plan,input,units,boundaries,neutral?rtl:true,em,storage,result,limits,steps,order,glyph_policy,cgj||(!logical&&controls)?&initial:nullptr,nullptr,!rtl);
    if(status!=SfntStatus::ok||rtl)return status;
    const u32 count=result->glyph_count;
    if(count>limits.stage_work){result->glyph_count=result->source_units=result->target_count=0;return SfntStatus::resource_limit;}
    for(u32 i=0;i<count;++i){auto& p=storage.positions[i];const u8 flags=storage.flags[i];
        const bool zero=(flags&sfnt_position_zero_advance)&&!plan.retain_zero_positioning;
        const double advance=zero?0:p.advance;
        auto& metadata=storage.glyphs[i].glyph.reserved;const bool control=(metadata&ltr_control_marker)!=0;metadata&=~ltr_control_marker;
        if(!neutral)p.advance_offset=(control?p.advance:advance)-storage.advances[i]-p.advance_offset;
        storage.flags[i]=(flags&sfnt_position_zero_advance)?1:0;
    }
    if(!neutral){reverse_prefix(storage.glyphs.data,0,count);reverse_prefix(storage.positions.data,0,count);reverse_prefix(storage.flags.data,0,count);}
    const auto clusters=sfnt_cluster_map_frame(storage.cluster_bits.data,units);
    const SfntGlyphView glyphs{reinterpret_cast<const SfntGsubGlyph*>(storage.glyphs.data),nullptr,count,0,sizeof(SfntComponentGlyph)};
    status=sfnt_cluster_map_export(clusters,glyphs,storage.character_glyphs,{},limits.stage_work);
    if(status==SfntStatus::ok)status=sfnt_position_flags_from_clusters(clusters,glyphs,{storage.flags.data,count},{storage.flags.data,count},limits.stage_work);
    if(status==SfntStatus::ok)status=sfnt_position_finalize({storage.positions.data,count},{storage.flags.data,count},em,plan.units_per_em,storage.placements,limits.stage_work,true,plan.retain_zero_positioning);
    if(status!=SfntStatus::ok)result->glyph_count=result->source_units=result->target_count=0;
    return status;
}
} // namespace maicord
