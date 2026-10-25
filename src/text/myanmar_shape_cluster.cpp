#include "maicord/text/myanmar_input.hpp"
#include "maicord/core/memory.hpp"
#include "myanmar_character.hpp"
#include "selector_slots.hpp"
#include "use_shape_internal.hpp"
namespace maicord {namespace {
// Windows overwrites only the current cluster's selector slots. A generated
// circle can observe a tail slot from an earlier cluster. Encode all 256
// selectors plus zero in 32 bytes and one high-bit mask instead of 128 bytes.
using SelectorSlots=SfntSelectorSlots;
SfntStatus map_cluster(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    ConstSlice<SfntIndicInputScalar> expanded,bool rtl,Slice<SfntClusterGlyph> mapped,u32& count,SelectorSlots* history) noexcept {
    count=0;u32 original=0;
    for(u32 i=0;i<expanded.count;++i){
        if(count==mapped.count)return SfntStatus::output_too_small;
        const auto current=expanded[i];u32 selector=0;
        // Windows copies selectors separately before reordering characters.
        // Selector slots keep their ordinal position, including when a circle
        // is inserted. Consume the original stream with an independent cursor.
        if(history&&count>=32)return SfntStatus::invalid_input;
        if(original<input.count){const u32 owner=input[original++];
            if(myanmar_category(owner)!='S'&&original<input.count&&myanmar_category(input[original])=='S')selector=input[original++];
            if(history)history->put(count,selector);}
        else if(history)selector=history->get(count);
        // Remove only the attached selector record from prepared output. Its
        // source association determines consumption, not variation selection.
        if(myanmar_category(current.scalar)!='S'&&i+1<expanded.count&&
            myanmar_category(expanded[i+1].scalar)=='S'&&
            expanded[i+1].source==current.source+(current.scalar>0xffff?2u:1u))++i;
        u16 glyph=0;const u32 scalar=rtl?unicode_bidi_mirror(current.scalar):current.scalar;
        auto status=sfnt_cmap_glyph(cmap,scalar,selector,&glyph);
        if(selector&&status==SfntStatus::unsupported_sequence)status=sfnt_cmap_glyph(cmap,scalar,0,&glyph);
        if(status!=SfntStatus::ok)return status;
        mapped[count++]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),0},current.source};
    }
    return SfntStatus::ok;
}

struct Work {
    u64 left,used=0;
    bool spend(u64 n) noexcept {if(n>left)return false;left-=n;used+=n;return true;}
};
struct RunPolicy {
    use_shape::GroupRun groups;const SfntShapePlan* plan;ConstSlice<SfntLookupStep> early;u64 edits=0;u32 match_work=0;
};
static_assert(sizeof(RunPolicy)==104);
SfntStatus run_prepare(void* context,u16 stage,SfntGlyphBuffer* buffer,SfntClusterMap* clusters,
    SfntComponentForest* forest,SfntGsubExecutionScratch scratch,u64 available,u64* used) noexcept {
    *used=0;if(stage)return SfntStatus::ok;
    auto& run=*static_cast<RunPolicy*>(context);forest->preserve_forms=false;
    if(!run.early.count)return SfntStatus::ok;
    Work w{available};struct Report {Work& w;u64* out;~Report() noexcept {*out=w.used;}} report{w,used};
    const auto seek=[&](SfntGlyphBuffer& value,u32 at) noexcept {
        const u64 cost=(at>value.gap_begin?at-value.gap_begin:value.gap_begin-at)+1ull;
        if(!w.spend(cost))return SfntStatus::resource_limit;return sfnt_glyph_buffer_seek(&value,at,cost);
    };
    for(u32 first=0;first<sfnt_glyph_count(sfnt_glyph_buffer_view(*buffer));){
        const auto view=sfnt_glyph_buffer_view(*buffer);const u32 total=sfnt_glyph_count(view);
        const auto& initial=*reinterpret_cast<const SfntComponentGlyph*>(&sfnt_glyph_at(view,first));
        const u32 group=run.groups.groups[initial.source];u32 end=first;
        while(end<total){const auto& value=*reinterpret_cast<const SfntComponentGlyph*>(&sfnt_glyph_at(view,end));
            if(!w.spend(1))return SfntStatus::resource_limit;if(run.groups.groups[value.source]!=group)break;++end;}
        auto status=seek(*buffer,end);if(status!=SfntStatus::ok)return status;
        // Expose only this cluster and the existing gap to the shared executor.
        // Both contextual lookahead and lookbehind stay inside the local view.
        SfntGlyphBuffer local{};
        status=sfnt_glyph_buffer_open({buffer->storage.data+first*sizeof(SfntComponentGlyph),
            (buffer->gap_end-first)*static_cast<u32>(sizeof(SfntComponentGlyph))},sizeof(SfntComponentGlyph),end-first,&local);
        if(status!=SfntStatus::ok)return status;
        for(u32 step=0;step<run.early.count;++step){
            const u32 count=sfnt_glyph_count(sfnt_glyph_buffer_view(local));
            if(run.plan->refresh_substitution_classes){if(!w.spend(count))return SfntStatus::resource_limit;
                auto* values=reinterpret_cast<SfntComponentGlyph*>(local.storage.data);
                for(u32 i=0;i<count;++i)values[i].glyph.glyph_class=sfnt_gdef_glyph_class(*run.groups.gdef,values[i].glyph.glyph);}
            SfntGsubExecutionResult execution{};
            status=sfnt_gsub_execute_lookup(*run.plan->substitutions,run.early[step].lookup,&local,0,count,0,scratch,
                {w.left,run.match_work},&execution,clusters,forest);
            if(!w.spend(execution.operations))return SfntStatus::invalid_input;run.edits+=execution.edits;
            if(status!=SfntStatus::ok)return status;
            status=seek(local,sfnt_glyph_count(sfnt_glyph_buffer_view(local)));if(status!=SfntStatus::ok)return status;
        }
        buffer->gap_begin=first+local.gap_begin;buffer->gap_end=first+local.gap_end;first=buffer->gap_begin;
    }
    return SfntStatus::ok;
}

}
SfntStatus sfnt_myanmar_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    float em,SfntShapeStorage storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,bool rtl,bool modern) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if((steps.count&&!steps.data)||(storage.positions.count&&!storage.positions.data)||
        (storage.character_glyphs.count&&!storage.character_glyphs.data)||(storage.targets.count&&!storage.targets.data)||
        plan.substitution_roots.count||(steps.count&&!plan.substitutions))return SfntStatus::invalid_input;
    if(steps.count>limits.stage_work)return SfntStatus::resource_limit;
    u32 early=0;
    for(u32 i=0;i<steps.count;++i){if(steps[i].stage>=7||steps[i].feature_mask||(i&&steps[i].stage<steps[i-1].stage)||
        steps[i].lookup>=plan.substitutions->lookups.count)return SfntStatus::invalid_input;if(steps[i].stage<5)++early;}
    auto status=sfnt_myanmar_cluster_boundaries(input,storage.flags,limits.stage_work);if(status!=SfntStatus::ok)return status;
    auto* expanded=reinterpret_cast<SfntIndicInputScalar*>(storage.positions.data);SfntIndicInputResult sizes{};
    const u64 slots=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntIndicInputScalar);
    if(modern){status=sfnt_myanmar_prepare_input(input,{expanded,static_cast<u32>(minimum<u64>(slots,0xffffffffu))},&sizes,limits.stage_work);
        if(status!=SfntStatus::ok)return status;
    }else{
        if(input.count>slots)return SfntStatus::output_too_small;
        // Boundary validation reserves input.count * 16 work, bounding this
        // direct mapping and its UTF-16 total within the same u32 allowance.
        for(u32 i=0;i<input.count;++i){expanded[i]={input[i],sizes.source_units};sizes.source_units+=input[i]>0xffff?2u:1u;}
        sizes.scalar_count=input.count;
    }
    if(sizes.source_units>storage.character_glyphs.count)return SfntStatus::output_too_small;
    auto* mapped=reinterpret_cast<SfntClusterGlyph*>(expanded);u32 count=0,units=0,starts=0,pieces=0;SelectorSlots selectors{};
    for(u32 first=0;first<input.count;){u32 end=first+1;while(end<input.count&&!storage.flags[end])++end;
        u32 end_units=units;for(u32 i=first;i<end;++i)end_units+=input[i]>0xffff?2u:1u;
        u32 end_pieces=pieces;while(end_pieces<sizes.scalar_count&&expanded[end_pieces].source<end_units)++end_pieces;
        if(starts==storage.targets.count)return SfntStatus::output_too_small;storage.targets[starts++]=units;
        for(u32 i=units;i<end_units;++i)storage.character_glyphs[i]=units;
        u32 amount=0;status=map_cluster(plan,cmap,{input.data+first,end-first},{expanded+pieces,end_pieces-pieces},rtl,
            {mapped+count,sizes.scalar_count-count},amount,modern?&selectors:nullptr);if(status!=SfntStatus::ok)return status;
        count+=amount;first=end;units=end_units;pieces=end_pieces;
    }
    RunPolicy policy{{&plan.gdef,0,{},{storage.character_glyphs.data,units},storage.targets,starts},&plan,{steps.data,early}};
    policy.match_work=limits.match_work;
    status=sfnt_cmap_glyph(cmap,0x20,0,&policy.groups.space);if(status!=SfntStatus::ok)return status;
    // USE recognizes the final nominal ZWNJ/ZWJ/LRM/RLM glyphs, including
    // aliases to .notdef. ZWSP is not in this set for Myanmar.
    for(u32 i=0;i<4;++i){status=sfnt_cmap_glyph(cmap,0x200c+i,0,&policy.groups.invisible[i]);if(status!=SfntStatus::ok)return status;}
    const SfntShapeStagePolicy stages{&policy,run_prepare,7};auto resolved=plan;resolved.retain_zero_positioning=true;
    status=sfnt_shape_run(resolved,{mapped,count},units,{storage.targets.data,starts},rtl,em,storage,result,limits,
        {steps.data?steps.data+early:nullptr,steps.count-early},{},{&policy,use_shape::finish_group_run,use_shape::restore_group_run},nullptr,&stages);
    result->substitution_edits+=policy.edits;return status;
}
}
