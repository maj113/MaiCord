#include "maicord/text/shaping/shape_run.hpp"
#include "maicord/core/memory.hpp"
#include "../opentype/gpos_validated.hpp"
#include "../opentype/gsub_validated.hpp"
#include "maicord/text/base.hpp"

namespace maicord {
namespace {
// A run's joiner-matching copy of a program, left uninitialized until a joined run fills it.
template<class Program> struct ProgramCopy {union {Program program;};ProgramCopy() noexcept {}};
// The copy of a program for one root of a run holding a joiner glyph: its GDEF names the font's joiners and those the
// root's feature skips (SfntGdef joiners). Out of line: the per-root loops of other runs carry only the branch.
template<class Program> __attribute__((noinline,cold)) const Program* joined_program(Program& copy,const Program& program,const SfntShapePlan& plan,u8 joiners) noexcept {
    copy=program;copy.gdef.zwj=plan.zwj;copy.gdef.zwnj=plan.zwnj;copy.gdef.joiners=joiners;return &copy;
}
// Whether the run's glyphs (summary) may hold the font's ZWJ or ZWNJ glyph.
bool holds_joiner(const SfntShapePlan& plan,const SfntGlyphSummary& glyphs) noexcept {
    return (plan.zwj&&sfnt_glyph_summary_has(glyphs,plan.zwj))||(plan.zwnj&&sfnt_glyph_summary_has(glyphs,plan.zwnj));
}
// joined: the run holds a joiner glyph; each root then runs on a copy of the program whose GDEF names the joiners
// GPOS skips: ZWNJ always, ZWJ unless the root's feature is F_MANUAL_ZWJ, and both in context.
}
SfntStatus shape_position_prepared(const SfntShapePlan& plan,SfntGlyphView glyphs,const SfntGlyphSummary& glyph_bounds,const SfntGlyphSummary& mark_bounds,
    u32 targets,float em,bool rtl,const SfntShapeStorage& s,SfntShapeRunResult* result,SfntShapeLimits limits,u64& work,bool defer_output,bool joined) noexcept {
    const u32 count=sfnt_glyph_count(glyphs);SfntStatus status=SfntStatus::ok;ProgramCopy<SfntGposProgram> copy;
    for(u32 index=0;index<plan.positioning_roots.count;++index){const u16 root=plan.positioning_roots[index];SfntGposExecutionResult execution{};
        // The first successful root validates the whole run. GPOS cannot edit glyph
        // IDs, classes, nominal metrics or component/source associations, and
        // successful position writes stay finite, so later disjoint roots need only
        // this bounded check. Only mark-class glyphs trigger attachment roots; base
        // glyph IDs must not widen their coverage test.
        const auto* prepared=sfnt_gpos_lookup(*plan.positioning,root);if(!prepared||!prepared->type)return SfntStatus::invalid_input;const auto& lookup=*prepared;
        const bool marks=lookup.type>=4&&lookup.type<=6;
        if(index&&((marks&&mark_bounds.first>mark_bounds.last)||sfnt_lookup_bounds_miss(lookup.bounds,marks?mark_bounds:glyph_bounds))){
            if(!work)return SfntStatus::resource_limit;--work;++result->lookup_operations;continue;
        }
        const SfntGposProgram* program=plan.positioning;
        if(joined)[[unlikely]]program=joined_program(copy.program,*plan.positioning,plan,static_cast<u8>(((sfnt_manual_joiners(plan.positioning_manual,root)&1u)?0u:sfnt_joiner_input_zwj)|
            sfnt_joiner_input_zwnj|sfnt_joiner_context_zwj|sfnt_joiner_context_zwnj));
        const auto execute=index?sfnt_gpos_execute_validated_lookup:sfnt_gpos_execute_program_lookup;
        status=execute(*program,root,glyphs,{s.flags.data,count},
            {s.associations.data,count},{s.targets.data,targets},{s.positions.data,count},0,count,rtl,plan.position_instance,
            SfntComponentAnchorPolicy::skip_missing,s.positioning,
            {work,limits.match_work},&execution);
        result->lookup_operations+=execution.operations;result->position_matches+=execution.matches;
        work-=execution.operations;if(status!=SfntStatus::ok)return status;
    }
    if(plan.legacy_kern.count){u64 left=work;status=sfnt_legacy_kern_apply(plan.legacy_kern,glyphs,{s.flags.data,count},{s.positions.data,count},rtl,&left);
        result->lookup_operations+=work-left;work=left;if(status!=SfntStatus::ok)return status;}
    if(!defer_output)status=sfnt_position_finalize({s.positions.data,count},{s.flags.data,count},em,plan.units_per_em,{s.placements.data,count},limits.stage_work,plan.retain_zero_positioning);
    return status;
}
namespace {
// Runs with per-cluster steps take the scoped instantiation; every other run keeps the plain one.
template<bool scoped> __attribute__((always_inline)) inline SfntStatus shape_run(const SfntShapePlan& plan,ConstSlice<SfntClusterGlyph> input,
    u32 units,ConstSlice<u32> boundaries,bool rtl,float em,const SfntShapeStorage& s,
    SfntShapeRunResult* result,SfntShapeLimits limits,ConstSlice<SfntLookupStep> steps,ConstSlice<u32> input_order,SfntShapeGlyphPolicy policy,const SfntShapeInputPolicy* initial,const SfntShapeStagePolicy* stages,bool defer_output) noexcept {
    if(!result)return SfntStatus::invalid_input;
    reset_value(result);
    if(!filled(input)||!filled(boundaries)||!filled(steps)||!filled(input_order)||!filled(plan.substitution_roots)||!filled(plan.positioning_roots)||
       !filled(s.glyphs)||!filled(s.components)||!filled(s.cluster_bits)||!filled(s.character_glyphs)||
       !filled(s.targets)||!filled(s.associations)||!filled(s.advances)||!filled(s.flags)||
       !filled(s.positions)||!filled(s.placements)||!plan.advance||!plan.units_per_em||
       !(em>0)||em>65536||(!input.count!=!units)||
       ((plan.substitution_roots.count||steps.count)&&!plan.substitutions)||(plan.positioning_roots.count&&!plan.positioning)||
       (steps.count&&plan.substitution_roots.count)||(input_order.count && input_order.count!=input.count))
        return SfntStatus::invalid_input;
    if(stages&&(!stages->prepare||!stages->count||stages->count>256||plan.substitution_roots.count))return SfntStatus::invalid_input;
    if constexpr(scoped)if(!stages->per_cluster.data||stages->per_cluster.count<(static_cast<u64>(steps.count)+63)/64)return SfntStatus::invalid_input;
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
    SfntGlyphSummary substitution{};
    for(u32 i=0;i<input.count;++i){const auto& source=input[input_order.count?input_order[i]:i];s.glyphs[i]={source.glyph,source.source,0,0};
        sfnt_glyph_summary_add(substitution,source.glyph.glyph);}
    status=sfnt_components_open({s.glyphs.data,input.count},s.components,&forest,limits.stage_work,steps.count!=0);
    if(status!=SfntStatus::ok)return status;
    status=sfnt_glyph_buffer_open({reinterpret_cast<u8*>(s.glyphs.data),s.glyphs.count*static_cast<u32>(sizeof(SfntComponentGlyph))},
        sizeof(SfntComponentGlyph),input.count,&buffer);
    if(status!=SfntStatus::ok)return status;
    if(initial){if(!initial->prepare)return SfntStatus::invalid_input;
        status=initial->prepare(initial->context,&buffer,&clusters,&forest,s.substitutions,limits.stage_work);if(status!=SfntStatus::ok)return status;
        const auto prepared=sfnt_glyph_buffer_view(buffer);const u32 count=sfnt_glyph_count(prepared);if(count>limits.stage_work)return SfntStatus::resource_limit;
        reset_value(&substitution);
        for(u32 i=0;i<count;++i)sfnt_glyph_summary_add(substitution,sfnt_glyph_at(prepared,i).glyph);
    }
    u64 work=limits.lookup_operations;SfntGlyphClassMemo classes;
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
        bounds_current=false;validated=validated&&stages->keeps_records;refresh=plan.refresh_substitution_classes;
        return SfntStatus::ok;
    };
    // A run holding a joiner glyph matches as HarfBuzz's apply context does: each root runs on a copy of the program
    // whose GDEF names the joiners GSUB skips: in input ZWJ unless F_MANUAL_ZWJ, in context ZWJ, and ZWNJ unless
    // F_MANUAL_ZWNJ. Substitution cannot create joiners from other glyphs, so the initial glyphs decide.
    const bool joined=holds_joiner(plan,substitution);
    ProgramCopy<SfntGsubProgram> copy;
    for(u32 index=0;index<roots;++index){const u16 root=steps.count?steps[index].lookup:plan.substitution_roots[index];
        const SfntGsubExecutableLookup* prepared=nullptr;status=sfnt_gsub_lookup_prepare(*plan.substitutions,root,&prepared);if(status!=SfntStatus::ok)return status;
        if(stages&&next_stage<=steps[index].stage){status=prepare_stages(steps[index].stage);if(status!=SfntStatus::ok)return status;}
        if(refresh){const auto current=sfnt_glyph_buffer_view(buffer);const u32 count=sfnt_glyph_count(current);
            if(count>work)return SfntStatus::resource_limit;work-=count;result->lookup_operations+=count;
            // Without GDEF glyph classes the input's (synthesized) classes stand, as in HarfBuzz.
            reset_value(&substitution);const bool classify=plan.gdef.glyph_classes.format!=0;
            for(u32 i=0;i<count;++i){auto& glyph=const_cast<SfntGsubGlyph&>(sfnt_glyph_at(current,i));if(classify)glyph.glyph_class=sfnt_gdef_glyph_class(plan.gdef,glyph.glyph,classes);
                sfnt_glyph_summary_add(substitution,glyph.glyph);}
            refresh=false;bounds_current=true;}
        // Substitutions preserve valid records but invalidate glyph bounds.
        // Script callbacks invalidate both; class refresh also restores bounds.
        if(validated&&bounds_current&&sfnt_lookup_bounds_miss(prepared->bounds,substitution)){
            if(!work)return SfntStatus::resource_limit;--work;++result->lookup_operations;continue;
        }
        const SfntGsubProgram* program=plan.substitutions;
        if(joined)[[unlikely]]{const u32 manual=sfnt_manual_joiners(plan.substitution_manual,root);
            program=joined_program(copy.program,*plan.substitutions,plan,static_cast<u8>(((manual&1u)?0u:sfnt_joiner_input_zwj)|sfnt_joiner_context_zwj|((manual&2u)?0u:sfnt_joiner_context_zwnj)));}
        SfntGsubExecutionResult execution{};
        const u32 end=sfnt_glyph_count(sfnt_glyph_buffer_view(buffer));const u8 mask=steps.count?steps[index].feature_mask:0;
        // Single-glyph inputs (types 1, 2, 3 and 8) cannot cross a cluster, so only ligature and contextual
        // lookups need the initial clusters as segments (validated by the cluster map).
        if constexpr(scoped)if(((stages->per_cluster[index>>6]>>(index&63))&1)&&prepared->type>=4&&prepared->type<=6&&boundaries.count){
            status=validated?sfnt_gsub_execute_validated_lookup(*program,*prepared,&buffer,0,end,0,s.substitutions,{work,limits.match_work},&execution,&clusters,&forest,mask,boundaries):
                sfnt_gsub_execute_lookup(*program,root,&buffer,0,end,0,s.substitutions,{work,limits.match_work},&execution,&clusters,&forest,mask,boundaries);
            result->lookup_operations+=execution.operations;result->substitution_edits+=execution.edits;
            work-=execution.operations;if(status!=SfntStatus::ok)return status;bounds_current=bounds_current&&!execution.edits;validated=true;
            refresh=plan.refresh_substitution_classes&&execution.edits!=0;continue;}
        status=validated?sfnt_gsub_execute_validated_lookup(*program,*prepared,&buffer,0,end,0,s.substitutions,{work,limits.match_work},&execution,&clusters,&forest,mask):
            sfnt_gsub_execute_lookup(*program,root,&buffer,0,end,0,s.substitutions,{work,limits.match_work},&execution,&clusters,&forest,mask);
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
    // One pass resolves final classes, lookup summaries, nominal advances,
    // cluster flags, initial positions and (unless deferred) first glyphs per
    // source cluster. Work bounds match the separate export/flag passes.
    if((!defer_output&&static_cast<u64>(units)*2+static_cast<u64>(count)*(clusters.levels*2+3)>limits.stage_work)||
       static_cast<u64>(count)*(clusters.levels*2+4)>limits.stage_work)return SfntStatus::resource_limit;
    constexpr u32 absent=0xffffffffu;
    if(!defer_output)for(u32 i=0;i<units;++i)s.character_glyphs[i]=absent;
    SfntGlyphSummary glyph_bounds{},mark_bounds{};u64 metric_slots=0;u32 previous=absent,previous_source=absent;
    const bool position_bounds=plan.positioning_roots.count>1;
    const u32 cache_size=plan.cache_advances?(s.targets.count>=64?64u:s.targets.count>=32?32u:0u):0u;
    for(u32 i=0;i<count;++i){auto& record=s.glyphs[i];auto& glyph=record.glyph;
        // Without GDEF glyph classes the input's (synthesized) classes stand, as in HarfBuzz.
        if(!policy.prepare){if(plan.gdef.glyph_classes.format)glyph.glyph_class=sfnt_gdef_glyph_class(plan.gdef,glyph.glyph,classes);}
        else if(s.flags[i]>1)return SfntStatus::invalid_input;
        if(static_cast<u8>(glyph.glyph_class)>4||record.source>=units)return SfntStatus::invalid_input;
        // Glyphs usually follow their sources; the next source unit either starts
        // a cluster or continues the previous one.
        const u32 source=record.source;
        const u32 cluster=source==previous_source?previous:source==previous_source+1?(sfnt_cluster_map_starts(clusters,source)?source:previous):
            sfnt_cluster_map_start(clusters,source);
        if(cluster==absent)return SfntStatus::invalid_input;previous_source=source;
        if(!defer_output&&s.character_glyphs[cluster]==absent)s.character_glyphs[cluster]=i;
        if(position_bounds){sfnt_glyph_summary_add(glyph_bounds,glyph.glyph);if(glyph.glyph_class==SfntGlyphClass::mark)sfnt_glyph_summary_add(mark_bounds,glyph.glyph);}
        const u32 slot=glyph.glyph&(cache_size?cache_size-1:0);const u64 bit=1ull<<slot;
        if(cache_size&&(metric_slots&bit)&&s.glyphs[s.targets[slot]].glyph.glyph==glyph.glyph)s.advances[i]=s.advances[s.targets[slot]];
        else{status=plan.advance(plan.metric_context,glyph.glyph,&s.advances[i]);if(status!=SfntStatus::ok)return status;
            if(cache_size){s.targets[slot]=i;metric_slots|=bit;}}
        const double advance=s.advances[i];if(!is_finite(advance))return SfntStatus::invalid_input;
        const bool zero=policy.prepare?s.flags[i]!=0:glyph.glyph_class==SfntGlyphClass::mark;
        s.flags[i]=static_cast<u8>((cluster!=previous?sfnt_position_cluster_start:0)|(zero?sfnt_position_zero_advance:0));previous=cluster;
        s.positions[i]=sfnt_position_nominal(advance,zero);
    }
    if(!defer_output)for(u32 i=0,glyph=absent;i<units;++i){
        if(sfnt_cluster_map_starts(clusters,i))glyph=s.character_glyphs[i];
        if(glyph==absent)return SfntStatus::invalid_input;s.character_glyphs[i]=glyph;
    }
    u32 targets=0;
    status=sfnt_components_export(&forest,glyphs,s.targets,s.associations,&targets,limits.stage_work);
    if(status!=SfntStatus::ok)return status;
    status=shape_position_prepared(plan,glyphs,glyph_bounds,mark_bounds,targets,em,rtl,s,result,limits,work,defer_output,joined);
    if(status==SfntStatus::ok){result->glyph_count=count;result->source_units=units;result->target_count=targets;}
    return status;
}
}
SfntStatus sfnt_shape_run(const SfntShapePlan& plan,ConstSlice<SfntClusterGlyph> input,
    u32 units,ConstSlice<u32> boundaries,bool rtl,float em,const SfntShapeStorage& s,
    SfntShapeRunResult* result,SfntShapeLimits limits,ConstSlice<SfntLookupStep> steps,ConstSlice<u32> input_order,SfntShapeGlyphPolicy policy,const SfntShapeInputPolicy* initial,const SfntShapeStagePolicy* stages,bool defer_output) noexcept {
    return stages&&stages->per_cluster.count?
        shape_run<true>(plan,input,units,boundaries,rtl,em,s,result,limits,steps,input_order,policy,initial,stages,defer_output):
        shape_run<false>(plan,input,units,boundaries,rtl,em,s,result,limits,steps,input_order,policy,initial,stages,defer_output);
}
}
