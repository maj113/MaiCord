#include "maicord/core/memory.hpp"
#include "selector_slots.hpp"
#include "maicord/text/tibetan_input.hpp"
#include "tibetan_character.hpp"
#include "use_shape_internal.hpp"
namespace maicord {
namespace {
SfntStatus map_cluster(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,bool rtl,
    Slice<SfntClusterGlyph> mapped,u32& count,u32& units,SfntSelectorSlots* history) noexcept {
    using C=TibetanCategory;using namespace use_shape;
    count=0;units=0;u32 selector_cursor=0;
    const auto selector=[](C c) noexcept {return c==C::selector||c==C::supplementary_selector;};
    const auto map=[&](u32 scalar,u32 source,u8 role) noexcept {
        if(count>=mapped.count)return SfntStatus::output_too_small;
        // Windows keeps selector slots in original non-selector order while
        // circles/expansion change the mapped order. Walk those slots once,
        // independently of source ownership, without a selector array.
        if(count>=32)return SfntStatus::invalid_input;
        u32 variation=0;
        if(selector_cursor<input.count){++selector_cursor;
            if(selector_cursor<input.count&&selector(tibetan_category(input[selector_cursor])))variation=input[selector_cursor++];
            history->put(count,variation);}
        else variation=history->get(count);
        const u32 code=rtl?unicode_bidi_mirror(scalar):scalar;u16 glyph=0;
        auto s=sfnt_cmap_glyph(cmap,code,variation,&glyph);
        if(variation&&s==SfntStatus::unsupported_sequence)s=sfnt_cmap_glyph(cmap,code,0,&glyph);
        if(s!=SfntStatus::ok)return s;
        if(!glyph&&unicode_properties(scalar)->general_category()==UnicodeGeneralCategory::cn)role|=control_role;
        mapped[count++]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),role},source};return SfntStatus::ok;
    };
    if(input.count){const auto first=tibetan_category(input[0]);
        if((first==C::subjoined||(first>=C::cons_above&&first<=C::split)||first==C::defective)&&
           !unicode_properties(input[0])->has(UnicodeFlag::default_ignorable_code_point)){
            auto status=map(0x25cc,0,base_role);if(status!=SfntStatus::ok)return status;}
    }
    for(u32 i=0;i<input.count;++i){const u32 original=input[i],source=units;units+=original>0xffff?2u:1u;
        if(i&&selector(tibetan_category(original)))continue;
        u32 pieces[2]{original,0};const u32 amount=tibetan_split_pair(original,pieces[0],pieces[1])?2u:1u;
        for(u32 piece=0;piece<amount;++piece){const auto scalar=pieces[piece];const auto c=tibetan_category(scalar);
            const u8 role=c==C::joiner||c==C::nonjoiner||c==C::cgj?
                static_cast<u8>(control_role|(c==C::joiner?join_role:c==C::cgj?cgj_role:0)):piece?0:
                c==C::base||c==C::placeholder?base_role:c==C::vowel_pre?pre_role:0;
            auto status=map(scalar,source,role);if(status!=SfntStatus::ok)return status;
        }
    }
    return SfntStatus::ok;
}
}
SfntStatus sfnt_tibetan_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    float em,SfntShapeStorage storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if((input.count&&!input.data)||(steps.count&&!steps.data)||plan.substitution_roots.count)return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*8+steps.count>limits.stage_work)return SfntStatus::resource_limit;
    if(steps.count&&!plan.substitutions)return SfntStatus::invalid_input;
    u32 early=0;
    for(u32 i=0;i<steps.count;++i){if(steps[i].feature_mask||steps[i].stage>=7||(i&&steps[i].stage<steps[i-1].stage)||
        steps[i].lookup>=plan.substitutions->lookups.count)return SfntStatus::invalid_input;if(steps[i].stage<5)++early;}
    u64 total_units=0;for(u32 i=0;i<input.count;++i){if(!unicode_is_scalar(input[i]))return SfntStatus::invalid_input;total_units+=input[i]>0xffff?2u:1u;}
    if(total_units>0xffffffffull)return SfntStatus::resource_limit;
    if(total_units>storage.character_glyphs.count)return SfntStatus::output_too_small;
    if((storage.character_glyphs.count&&!storage.character_glyphs.data)||(storage.positions.count&&!storage.positions.data)||
        (storage.targets.count&&!storage.targets.data))return SfntStatus::invalid_input;
    // Reuse positions for initial mapping. Each complete cluster commits its
    // original group IDs before shared execution takes over this storage.
    auto* mapped=reinterpret_cast<SfntClusterGlyph*>(storage.positions.data);
    const u32 capacity=static_cast<u32>(minimum<u64>(static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntClusterGlyph),0xffffffffu));
    u32 count=0,units=0,starts=0;SfntSelectorSlots history{};
    for(u32 first=0;first<input.count;){const u32 window=minimum<u32>(63,input.count-first);u8 boundaries[63];
        auto status=sfnt_tibetan_cluster_boundaries({input.data+first,window},{boundaries,63},limits.stage_work);if(status!=SfntStatus::ok)return status;
        const u32 origin=first;
        for(u32 at=0;at<window;){u32 length=1;while(at+length<window&&!boundaries[at+length])++length;
            if(at+length==window&&origin+window<input.count)break;
            if(starts>=storage.targets.count)return SfntStatus::output_too_small;
            u32 amount=0,source_units=0;
            status=map_cluster(plan,cmap,{input.data+origin+at,length},rtl,{mapped?mapped+count:nullptr,capacity-count},amount,source_units,&history);if(status!=SfntStatus::ok)return status;
            storage.targets[starts++]=units;
            for(u32 i=0;i<amount;++i)mapped[count+i].source+=units;
            for(u32 i=0;i<source_units;++i)storage.character_glyphs[units+i]=units;
            count+=amount;units+=source_units;at+=length;first=origin+at;
        }
    }
    u16 controls[4];for(u32 i=0;i<4;++i){const auto status=sfnt_cmap_glyph(cmap,0x200c+i,0,controls+i);if(status!=SfntStatus::ok)return status;}
    return use_shape::run(plan,cmap,{mapped,count},units,starts,early,0,em,storage,result,steps,limits,rtl,controls);
}
}
