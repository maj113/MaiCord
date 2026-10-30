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
        const auto status=use_shape::map_glyph(plan,cmap,current.scalar,selector,rtl,0,current.source,&mapped[count]);
        if(status!=SfntStatus::ok)return status;
        ++count;
    }
    return SfntStatus::ok;
}
}
SfntStatus sfnt_myanmar_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,const SfntShapeLimits& limits,bool rtl,bool modern) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    u32 early=0;
    if(const auto admitted=use_shape::admit_run(plan,input,steps,storage,limits,&early);admitted!=SfntStatus::ok)return admitted;
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
    return use_shape::run(use_shape::Script::myanmar,plan,cmap,{mapped,count},units,starts,early,em,storage,result,steps,limits,rtl);
}
}
