#include "maicord/text/shape_run.hpp"
#include "gpos_validated.hpp"
#include "gsub_validated.hpp"
#include "shape_position.hpp"

namespace maicord {
namespace {
template<class T> bool valid(Slice<T> s) noexcept {return !s.count||s.data;}
template<class T> bool valid(ConstSlice<T> s) noexcept {return !s.count||s.data;}
}
SfntStatus shape_position_prepared(const SfntShapePlan& plan,SfntGlyphView glyphs,ShapeGlyphBounds glyph_bounds,ShapeGlyphBounds mark_bounds,
    u32 targets,float em,bool rtl,SfntShapeStorage s,SfntShapeRunResult* result,SfntShapeLimits limits,u64& work,bool defer_output) noexcept {
    const u32 count=sfnt_glyph_count(glyphs);SfntStatus status=SfntStatus::ok;
    for(u32 index=0;index<plan.positioning_roots.count;++index){const u16 root=plan.positioning_roots[index];SfntGposExecutionResult execution{};
        // The first successful root validates the whole run. GPOS cannot edit glyph
        // IDs, classes, nominal metrics or component/source associations, and
        // successful position writes stay finite, so later disjoint roots need only
        // this bounded check. Only mark-class glyphs trigger attachment roots; base
        // glyph IDs must not widen their coverage test.
        const auto* prepared=sfnt_gpos_lookup(*plan.positioning,root);if(!prepared||!prepared->type)return SfntStatus::invalid_input;const auto& lookup=*prepared;
        const bool marks=lookup.type>=4&&lookup.type<=6;
        if(index&&((marks&&mark_bounds.first>mark_bounds.last)||sfnt_lookup_bounds_disjoint(lookup.bounds,marks?mark_bounds.first:glyph_bounds.first,marks?mark_bounds.last:glyph_bounds.last))){
            if(!work)return SfntStatus::resource_limit;--work;++result->lookup_operations;continue;
        }
        const auto execute=index?sfnt_gpos_execute_validated_lookup:sfnt_gpos_execute_program_lookup;
        status=execute(*plan.positioning,root,glyphs,{s.advances.data,count},{s.flags.data,count},
            {s.associations.data,count},{s.targets.data,targets},{s.positions.data,count},0,count,rtl,plan.position_instance,
            SfntNullAnchorPolicy::windows_parent_coordinates,SfntComponentAnchorPolicy::skip_missing,s.positioning,
            {work,limits.match_work},&execution,plan.anchor_point,plan.anchor_context);
        result->lookup_operations+=execution.operations;result->position_matches+=execution.matches;
        work-=execution.operations;if(status!=SfntStatus::ok)return status;
    }
    if(!defer_output)status=sfnt_position_finalize({s.positions.data,count},{s.flags.data,count},em,plan.units_per_em,{s.placements.data,count},limits.stage_work,plan.compensate_consecutive_zeroes,plan.retain_zero_positioning);
    return status;
}
SfntStatus sfnt_shape_run(const SfntShapePlan& plan,ConstSlice<SfntClusterGlyph> input,
    u32 units,ConstSlice<u32> boundaries,bool rtl,float em,SfntShapeStorage s,
    SfntShapeRunResult* result,SfntShapeLimits limits,ConstSlice<SfntLookupStep> steps,ConstSlice<u32> input_order,SfntShapeGlyphPolicy policy,const SfntShapeInputPolicy* initial,const SfntShapeStagePolicy* stages,bool defer_output) noexcept {
    if(!result)return SfntStatus::invalid_input;
    *result={};
    if(!valid(input)||!valid(boundaries)||!valid(steps)||!valid(input_order)||!valid(plan.substitution_roots)||!valid(plan.positioning_roots)||
       !valid(s.glyphs)||!valid(s.components)||!valid(s.cluster_bits)||!valid(s.character_glyphs)||
       !valid(s.targets)||!valid(s.associations)||!valid(s.advances)||!valid(s.flags)||
       !valid(s.positions)||!valid(s.placements)||!plan.advance||!plan.units_per_em||
       !(em>0)||em>65536||(!input.count!=!units)||
       ((plan.substitution_roots.count||steps.count)&&!plan.substitutions)||(plan.positioning_roots.count&&!plan.positioning)||
       (steps.count&&plan.substitution_roots.count)||(input_order.count && input_order.count!=input.count))
        return SfntStatus::invalid_input;
    if(stages&&(!stages->prepare||!stages->count||stages->count>256||plan.substitution_roots.count))return SfntStatus::invalid_input;
    if(input.count>limits.stage_work||steps.count>limits.stage_work||plan.substitution_roots.count>limits.stage_work||
       plan.positioning_roots.count>limits.stage_work)return SfntStatus::resource_limit;
    if(input.count>s.glyphs.count||units>s.character_glyphs.count||s.glyphs.count>0xffffffffu/sizeof(SfntComponentGlyph))
        return SfntStatus::output_too_small;
    for(u32 i=0;i<input.count;++i)if(input[i].source>=units)return SfntStatus::invalid_input;
    for(u32 i=0;i<input_order.count;++i)if(input_order[i]>=input.count)return SfntStatus::invalid_input;
    for(u32 i=0;i<plan.substitution_roots.count;++i)if(plan.substitution_roots[i]>=plan.substitutions->lookups.count)return SfntStatus::invalid_input;
    for(u32 i=0;i<steps.count;++i)if(steps[i].lookup>=plan.substitutions->lookups.count || steps[i].stage>=(stages?stages->count:3) || (i && steps[i].stage<steps[i-1].stage))return SfntStatus::invalid_input;
    for(u32 i=0;i<plan.positioning_roots.count;++i)if(plan.positioning_roots[i]>=plan.positioning->lookups.count)return SfntStatus::invalid_input;
    if(!input.count)return boundaries.count?SfntStatus::invalid_input:SfntStatus::ok;
    SfntClusterMap clusters{};SfntComponentForest forest{};SfntGlyphBuffer buffer{};
    auto status=sfnt_cluster_map_open(units,boundaries,s.cluster_bits,&clusters,limits.stage_work);
    if(status!=SfntStatus::ok)return status;
    u16 substitution_first=65535,substitution_last=0;
    for(u32 i=0;i<input.count;++i){const auto& source=input[input_order.count?input_order[i]:i];s.glyphs[i]={source.glyph,source.source,0,0};
        substitution_first=minimum(substitution_first,source.glyph.glyph);substitution_last=maximum(substitution_last,source.glyph.glyph);}
    status=sfnt_components_open({s.glyphs.data,input.count},s.components,&forest,limits.stage_work,steps.count!=0);
    if(status!=SfntStatus::ok)return status;
    status=sfnt_glyph_buffer_open({reinterpret_cast<u8*>(s.glyphs.data),s.glyphs.count*static_cast<u32>(sizeof(SfntComponentGlyph))},
        sizeof(SfntComponentGlyph),input.count,&buffer);
    if(status!=SfntStatus::ok)return status;
    if(initial){if(!initial->prepare)return SfntStatus::invalid_input;
        status=initial->prepare(initial->context,&buffer,&clusters,&forest,s.substitutions,limits.stage_work);if(status!=SfntStatus::ok)return status;
        const auto prepared=sfnt_glyph_buffer_view(buffer);const u32 count=sfnt_glyph_count(prepared);if(count>limits.stage_work)return SfntStatus::resource_limit;
        substitution_first=65535;substitution_last=0;
        for(u32 i=0;i<count;++i){const auto glyph=sfnt_glyph_at(prepared,i).glyph;substitution_first=minimum(substitution_first,glyph);substitution_last=maximum(substitution_last,glyph);}
    }
    u64 work=limits.lookup_operations;
    const u32 roots=steps.count?steps.count:plan.substitution_roots.count;bool bounds_current=true,refresh=false,validated=false;
    u16 next_stage=0;
    const auto prepare_stages=[&](u16 last) noexcept {
        while(next_stage<=last){
            if(!work)return SfntStatus::resource_limit;--work;++result->lookup_operations;
            const u64 available=minimum(work,static_cast<u64>(limits.stage_work));u64 used=0;
            const auto prepared=stages->prepare(stages->context,next_stage++,&buffer,&clusters,&forest,s.substitutions,available,&used);
            if(used>available)return SfntStatus::invalid_input;
            work-=used;result->lookup_operations+=used;if(prepared!=SfntStatus::ok)return prepared;
        }
        // A script callback may change glyph IDs, order or component metadata
        // without a GSUB edit. Invalidate the first-root/bounds shortcut too.
        bounds_current=false;validated=false;refresh=plan.refresh_substitution_classes;
        return SfntStatus::ok;
    };
    for(u32 index=0;index<roots;++index){const u16 root=steps.count?steps[index].lookup:plan.substitution_roots[index];
        const SfntGsubExecutableLookup* prepared=nullptr;status=sfnt_gsub_lookup_prepare(*plan.substitutions,root,&prepared);if(status!=SfntStatus::ok)return status;
        if(stages&&next_stage<=steps[index].stage){status=prepare_stages(steps[index].stage);if(status!=SfntStatus::ok)return status;}
        if(refresh){const auto current=sfnt_glyph_buffer_view(buffer);const u32 count=sfnt_glyph_count(current);
            if(count>work)return SfntStatus::resource_limit;work-=count;result->lookup_operations+=count;
            substitution_first=65535;substitution_last=0;
            for(u32 i=0;i<count;++i){auto& glyph=const_cast<SfntGsubGlyph&>(sfnt_glyph_at(current,i));glyph.glyph_class=sfnt_gdef_glyph_class(plan.gdef,glyph.glyph);
                substitution_first=minimum(substitution_first,glyph.glyph);substitution_last=maximum(substitution_last,glyph.glyph);}
            refresh=false;bounds_current=true;}
        // Substitutions preserve valid records but invalidate glyph bounds.
        // Script callbacks invalidate both; class refresh also restores bounds.
        if(validated&&bounds_current&&sfnt_lookup_bounds_disjoint(prepared->bounds,substitution_first,substitution_last)){
            if(!work)return SfntStatus::resource_limit;--work;++result->lookup_operations;continue;
        }
        SfntGsubExecutionResult execution{};
        const u32 end=sfnt_glyph_count(sfnt_glyph_buffer_view(buffer));const u8 mask=steps.count?steps[index].feature_mask:0;
        status=validated?sfnt_gsub_execute_validated_lookup(*plan.substitutions,*prepared,&buffer,0,end,0,s.substitutions,{work,limits.match_work},&execution,&clusters,&forest,mask):
            sfnt_gsub_execute_lookup(*plan.substitutions,root,&buffer,0,end,0,s.substitutions,{work,limits.match_work},&execution,&clusters,&forest,mask);
        result->lookup_operations+=execution.operations;result->substitution_edits+=execution.edits;
        work-=execution.operations;if(status!=SfntStatus::ok)return status;bounds_current=bounds_current&&!execution.edits;validated=true;
        refresh=plan.refresh_substitution_classes&&execution.edits!=0;
    }
    if(stages&&next_stage<stages->count){status=prepare_stages(static_cast<u16>(stages->count-1));if(status!=SfntStatus::ok)return status;}
    const u32 count=sfnt_glyph_count(sfnt_glyph_buffer_view(buffer));
    if(count>limits.stage_work)return SfntStatus::resource_limit;
    if(count>s.flags.count||count>s.advances.count||count>s.positions.count||count>s.placements.count||
       count>s.associations.count)return SfntStatus::output_too_small;
    status=sfnt_glyph_buffer_seek(&buffer,count,limits.stage_work);if(status!=SfntStatus::ok)return status;
    const auto glyphs=sfnt_glyph_buffer_view(buffer);
    if(policy.prepare){status=policy.prepare(policy.context,{s.glyphs.data,count},{s.flags.data,count},limits.stage_work);if(status!=SfntStatus::ok)return status;}
    if(policy.clusters){status=policy.clusters(policy.context,&clusters,forest,{s.glyphs.data,count},limits.stage_work);if(status!=SfntStatus::ok)return status;}
    if(!defer_output)status=sfnt_cluster_map_export(clusters,glyphs,s.character_glyphs,{},limits.stage_work);
    if(status!=SfntStatus::ok)return status;
    u16 first_glyph=65535,last_glyph=0,first_mark=65535,last_mark=0;u64 metric_slots=0;
    const bool position_bounds=plan.positioning_roots.count>1;
    const u32 cache_size=plan.cache_advances?(s.targets.count>=64?64u:s.targets.count>=32?32u:0u):0u;
    for(u32 i=0;i<count;++i){auto& glyph=s.glyphs[i].glyph;
        if(!policy.prepare)glyph.glyph_class=sfnt_gdef_glyph_class(plan.gdef,glyph.glyph);
        if(position_bounds){first_glyph=minimum(first_glyph,glyph.glyph);last_glyph=maximum(last_glyph,glyph.glyph);
            if(glyph.glyph_class==SfntGlyphClass::mark){first_mark=minimum(first_mark,glyph.glyph);last_mark=maximum(last_mark,glyph.glyph);}}
        const u32 slot=glyph.glyph&(cache_size?cache_size-1:0);const u64 bit=1ull<<slot;
        if(cache_size&&(metric_slots&bit)&&s.glyphs[s.targets[slot]].glyph.glyph==glyph.glyph)s.advances[i]=s.advances[s.targets[slot]];
        else{status=plan.advance(plan.metric_context,glyph.glyph,&s.advances[i]);if(status!=SfntStatus::ok)return status;
            if(cache_size){s.targets[slot]=i;metric_slots|=bit;}}
    }
    status=sfnt_position_flags_from_clusters(clusters,glyphs,{s.flags.data,count},policy.prepare?ConstSlice<u8>{s.flags.data,count}:ConstSlice<u8>{},limits.stage_work);
    if(status!=SfntStatus::ok)return status;
    u32 targets=0;
    status=sfnt_components_export(&forest,glyphs,s.targets,s.associations,&targets,limits.stage_work);
    if(status!=SfntStatus::ok)return status;
    status=sfnt_position_initialize({s.advances.data,count},{s.flags.data,count},rtl,{s.positions.data,count},limits.stage_work);
    if(status!=SfntStatus::ok)return status;
    status=shape_position_prepared(plan,glyphs,{first_glyph,last_glyph},{first_mark,last_mark},targets,em,rtl,s,result,limits,work,defer_output);
    if(status==SfntStatus::ok){result->glyph_count=count;result->source_units=units;result->target_count=targets;}
    return status;
}
}
