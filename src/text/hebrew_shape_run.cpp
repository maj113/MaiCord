#include "maicord/text/hebrew_shape_run.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/unicode.hpp"
namespace maicord {namespace {
bool control(u32 cp) noexcept {return cp==0x34f||cp==0x200c||cp==0x200d;}
bool base(u32 cp) noexcept {return cp==0x5c6||((cp-0x5d0u<35||cp-0xfb1du<51)&&unicode_properties(cp)->general_category()==UnicodeGeneralCategory::lo);}
u64 mark(u32 cp) noexcept {
    if(cp==0xfb1e)return 1ull<<55;
    return (cp-0x591u<45||cp==0x5bf||cp==0x5c1||cp==0x5c2||cp==0x5c4||cp==0x5c5||cp==0x5c7)?1ull<<(cp-0x591u):0;
}
u32 embedded_mark(u32 cp) noexcept {
    if(cp==0xfb1d)return 0x5b4;if(cp==0xfb1f||cp==0xfb2e)return 0x5b7;if(cp==0xfb2f)return 0x5b8;
    if(cp-0x5f0u<3||cp==0xfb2c||cp==0xfb2d||cp-0xfb30u<27)return 0x5bc;
    if(cp==0xfb4b)return 0x5b9;if(cp-0xfb4cu<3)return 0x5bf;return 0;
}
u64 conflicts(u32 cp) noexcept {
    if(cp-0x5b1u<3)return mark(0x5b0)|mark(0x5b1)|mark(0x5b2)|mark(0x5b3)|mark(0x5b4)|mark(0x5b5)|mark(0x5b6)|mark(0x5b7)|mark(0x5b8)|mark(0x5bb)|mark(0x5c7);
    if(cp==0x5b9||cp==0x5ba)return mark(0x5b9)|mark(0x5ba);
    if(cp==0x5c1||cp==0x5c2)return mark(0x5c1)|mark(0x5c2);
    const bool vowel=cp==0x5b0||cp-0x5b4u<5||cp==0x5bb||cp==0x5c7;
    return mark(cp)|(vowel?mark(0x5b1)|mark(0x5b2)|mark(0x5b3):0);
}
u32 rank(u32 cp) noexcept {
    if(cp==0x5c1||cp==0x5c2)return 0;
    if(cp==0x5bc||cp==0x5bf)return 1;
    if(cp==0x5b9||cp==0x5ba)return 2;
    if(cp==0x5ae)return 6;if(cp==0x5af)return 7;
    const auto value=unicode_properties(cp)->combining_class();return value==222?4u:value==230?5u:3u;
}
struct Policy {const SfntGdef* gdef;const u32* sources;u32 units;u16 space,controls[3];};
constexpr u8 dotted_circle=128,substituted_control=64;
SfntStatus finish(void* context,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    const auto& policy=*static_cast<const Policy*>(context);if(glyphs.count>work)return SfntStatus::resource_limit;
    if(zero.count<glyphs.count)return SfntStatus::output_too_small;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];if(g.source>=policy.units)return SfntStatus::invalid_input;
        const u8 type=(g.glyph.reserved&dotted_circle)?0:policy.sources[g.source]&3;
        const bool original=type&&g.glyph.glyph==policy.controls[type-1],missing=!g.glyph.glyph;
        if(type&&!original&&!missing)g.glyph.reserved|=substituted_control;
        if((original&&type!=2)||missing)g.glyph.glyph=policy.space;
        g.glyph.glyph_class=sfnt_gdef_glyph_class(*policy.gdef,g.glyph.glyph);
        zero[i]=static_cast<u8>((original&&type!=2)||missing||g.glyph.glyph_class==SfntGlyphClass::mark);}
    return SfntStatus::ok;
}
SfntStatus repair(void* context,SfntClusterMap* clusters,const SfntComponentForest& forest,Slice<SfntComponentGlyph> glyphs,u64 work) noexcept {
    const auto& policy=*static_cast<const Policy*>(context);
    for(u32 i=0;i<glyphs.count;++i){if(!work)return SfntStatus::resource_limit;--work;auto& g=glyphs[i];
        if(g.source>=policy.units)return SfntStatus::invalid_input;
        if(g.glyph.reserved&substituted_control){const u32 first=policy.sources[g.source]>>3;
            if(first<g.source){SfntClusterMerge merge{};u64 used=0;auto status=sfnt_cluster_map_plan(*clusters,first,g.source,&merge,work,&used);work-=used;if(status!=SfntStatus::ok)return status;
                if(merge.commit_work>work)return SfntStatus::resource_limit;work-=merge.commit_work;sfnt_cluster_map_commit(clusters,merge);}
        }
        if(g.glyph.glyph_class!=SfntGlyphClass::mark||g.component==0xffffffffu||(policy.sources[g.source]&4))continue;
        u32 lo=0,hi=0;u64 used=0;auto status=sfnt_component_source_bounds(forest,g.component,&lo,&hi,work,&used);work-=used;if(status!=SfntStatus::ok)return status;
        if(hi>=policy.units)return SfntStatus::invalid_input;
        hi=policy.sources[hi]>>3;
        if(i&&hi>g.source){status=sfnt_cluster_map_split(clusters,hi,work);if(status!=SfntStatus::ok)return status;work-=clusters->levels;g.source=hi;}}
    return SfntStatus::ok;
}
}
SfntStatus sfnt_hebrew_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntClusterGlyph> input,ConstSlice<u32> scalars,
    u32 units,bool rtl,float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,SfntShapeLimits limits) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(input.count!=scalars.count||(input.count&&(!input.data||!scalars.data))||!storage.substitutions.replacements.data||!storage.targets.data||!storage.advances.data)return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*10+static_cast<u64>(units)*2>limits.stage_work||units>0x1fffffffu)return SfntStatus::resource_limit;
    if(static_cast<u64>(storage.advances.count)*2<units)return SfntStatus::output_too_small;
    const u64 capacity=storage.substitutions.replacements.count/sizeof(SfntClusterGlyph);
    if(reinterpret_cast<usize>(storage.substitutions.replacements.data)%alignof(SfntClusterGlyph))return SfntStatus::invalid_input;
    auto* expanded=reinterpret_cast<SfntClusterGlyph*>(storage.substitutions.replacements.data);auto* sources=reinterpret_cast<u32*>(storage.advances.data);
    for(u32 i=0;i<units;++i)sources[i]=0;
    for(u32 i=0;i<input.count;++i){if(input[i].source>=units||(i&&input[i].source<input[i-1].source))return SfntStatus::invalid_input;
        if(control(scalars[i]))sources[input[i].source]=static_cast<u8>(scalars[i]==0x34f?2:scalars[i]==0x200c?1:3);}
    u16 circle=0,space=0;auto status=sfnt_cmap_glyph(cmap,0x25cc,0,&circle);if(status!=SfntStatus::ok)return status;
    status=sfnt_cmap_glyph(cmap,32,0,&space);if(status!=SfntStatus::ok)return status;
    u32 written=0,starts=0,control_end=0;bool broken_context=false,controls=false;
    for(u32 first=0;first<input.count;){const u32 cp=scalars[first];const bool letter=base(cp),holam=cp==0x5b9||cp==0x5ba;
        if(first>=control_end){control_end=first+1;controls=control(cp);
            while(control_end<input.count){const u32 next=scalars[control_end];if(!control(next)&&!mark(next))break;
                controls|=control(next);++control_end;}}
        const bool broken=(mark(cp)!=0&&(!holam||first!=0))||(control(cp)&&broken_context);
        const u32 embedded=embedded_mark(cp);u64 used=mark(embedded)|(cp==0xfb1f?mark(0x5bc):0);u32 end=first+1,accents=0,vowels=embedded-0x5b0u<9?1u:0u;
        while(end<input.count){const u32 next=scalars[end];if(control(next)&&!broken){controls=true;++end;continue;}
            const bool masora=next==0x5af&&(cp==0x5be||cp==0x5c0||cp==0x5c3||cp==0x5f3||cp==0x5f4||cp==0xfb29);
            const u64 bit=mark(next);if((!letter&&!masora)||!bit||(used&conflicts(next))||((next==0x5c1||next==0x5c2)&&cp!=0x5e9))break;
            if(next<0x5af||next==0x5bd){if(accents==2)break;++accents;}
            if(next-0x5b0u<9||next==0x5bb||next==0x5c7){const u32 amount=next-0x5b1u<3?2u:1u;if(vowels+amount>2)break;vowels+=amount;}
            used|=bit;++end;}
        if(starts==storage.targets.count||static_cast<u64>(written)+end-first+(broken?1u:0u)>capacity)return SfntStatus::output_too_small;
        storage.targets[starts++]=input[first].source;
        sources[input[first].source]|=4;
        if(broken)expanded[written++]={{circle,sfnt_gdef_glyph_class(plan.gdef,circle),dotted_circle},input[first].source};
        expanded[written++]=input[first];
        u32 limit=first+1,buckets[8]{};
        while(!controls&&limit<end){const u32 priority=rank(scalars[limit]);if((priority==1||priority==4)&&buckets[priority])break;
            ++buckets[priority];++limit;}
        u32 cursor=written;for(u32 group=0;group<8;++group){const u32 count=buckets[group];buckets[group]=cursor;cursor+=count;}
        for(u32 i=first+1;i<limit;++i)expanded[buckets[rank(scalars[i])]++]=input[i];written=cursor;
        for(u32 i=limit;i<end;++i)expanded[written++]=input[i];
        broken_context=broken||used!=0||mark(cp)!=0;first=end;
    }
    // Pack the preceding initial boundary beside control flags in borrowed
    // advance scratch. Later source repairs use one indexed read per glyph,
    // rather than rescanning long control or combining sequences.
    u32 initial=0;for(u32 i=0;i<units;++i){if(sources[i]&4)initial=i;sources[i]|=initial<<3;}
    Policy policy{&plan.gdef,sources,units,space,{}};const u32 control_scalars[]{0x200c,0x34f,0x200d};
    for(u32 i=0;i<3;++i){status=sfnt_cmap_glyph(cmap,control_scalars[i],0,policy.controls+i);if(status!=SfntStatus::ok)return status;}
    return sfnt_shape_run(plan,{expanded,written},units,{storage.targets.data,starts},rtl,em,storage,result,limits,{}, {},{&policy,finish,repair});
}
}
