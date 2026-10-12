#include "maicord/text/khmer_shape_cluster.hpp"
#include "maicord/text/khmer_shape_run.hpp"
#include "maicord/text/khmer_input.hpp"
#include "maicord/text/khmer_plan.hpp"
#include "khmer_character.hpp"
namespace maicord {namespace {
using C=KhmerCategory;
constexpr u32 nil=0xffffffffu;
constexpr u8 pre_role=4,ro_first=8,ro_last=16,generated_pre=64;
struct Policy {const SfntGdef* gdef;u32 units=0;u16 space=0,zero[3]{};};
static_assert(sizeof(Policy)==24);
SfntStatus reorder(Slice<SfntClusterGlyph> glyphs,u64 available,u64* spent=nullptr) noexcept {
    u64 used=0;const auto spend=[&](u64 cost) noexcept {if(cost>available-used)return false;used+=cost;return true;};
    struct Report {u64& used;u64* out;~Report() noexcept {if(out)*out=used;}} report{used,spent};
    const u32 n=glyphs.count;
    // Reorder in original event order. A literal pre-vowel protects its
    // prefix from later RO pairs; a generated piece alone does not. Generated
    // pieces prepend and extend an already protected literal-vowel prefix.
    u32 protected_prefix=0;bool moved_ro=false;
    for(u32 i=0;i<n;){if(!spend(1))return SfntStatus::resource_limit;
        const u8 role=glyphs[i].glyph.reserved;
        if(role&ro_first){
            if(i+1>=n||!(glyphs[i+1].glyph.reserved&ro_last)||protected_prefix>i)return SfntStatus::invalid_input;
            if(!spend(i-protected_prefix+2ull))return SfntStatus::resource_limit;
            const auto a=glyphs[i],b=glyphs[i+1];
            for(u32 j=i;j>protected_prefix;--j)glyphs[j+1]=glyphs[j-1];
            glyphs[protected_prefix]=a;glyphs[protected_prefix+1]=b;moved_ro=true;i+=2;
        }else{
            if(role&pre_role){
                if(!spend(i+1ull))return SfntStatus::resource_limit;
                const auto saved=glyphs[i];for(u32 j=i;j;--j)glyphs[j]=glyphs[j-1];glyphs[0]=saved;
                if(role&generated_pre){if(protected_prefix||moved_ro)++protected_prefix;}else protected_prefix=1;
            }++i;
        }
    }
    return SfntStatus::ok;
}
SfntStatus finish(void* context,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    auto& p=*static_cast<Policy*>(context);
    if(zero.count<glyphs.count)return SfntStatus::output_too_small;
    if(glyphs.count>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<glyphs.count;++i){auto& item=glyphs[i];auto& g=item.glyph;
        // Khmer postprocessing treats the font's three nominal zero-width glyph IDs
        // as invisible, even when another scalar maps to the same ID; a
        // substitution to a different ID stays visible.
        const bool invisible=g.glyph==p.zero[0]||g.glyph==p.zero[1]||g.glyph==p.zero[2];
        if(invisible)g.glyph=p.space;g.glyph_class=sfnt_gdef_glyph_class(*p.gdef,g.glyph);g.reserved=static_cast<u8>(invisible);
        zero[i]=static_cast<u8>(invisible||g.glyph_class==SfntGlyphClass::mark);
    }return SfntStatus::ok;
}
SfntStatus restore(void* context,SfntClusterMap* clusters,const SfntComponentForest&,Slice<SfntComponentGlyph>,u64 work) noexcept {
    auto& p=*static_cast<Policy*>(context);SfntClusterMerge merge{};u64 used=0;
    const auto status=sfnt_cluster_map_plan(*clusters,0,p.units-1,&merge,work,&used);if(status!=SfntStatus::ok)return status;
    if(merge.commit_work>work-used)return SfntStatus::resource_limit;sfnt_cluster_map_commit(clusters,merge);return SfntStatus::ok;
}
SfntStatus map_cluster(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    ConstSlice<SfntKhmerInputScalar> expanded,bool rtl,Slice<SfntClusterGlyph> mapped,u32& count,u32 source_base=0) noexcept {
    count=0;bool seen_ro=false,cfar=false;u32 pending=nil;
    const auto map=[&](u32 scalar,u32 source,u8 role,u32 selector=0) noexcept {
        if(count>=mapped.count)return SfntStatus::output_too_small;
        u16 glyph=0;const u32 resolved=rtl?unicode_bidi_mirror(scalar):scalar;
        auto status=sfnt_cmap_glyph(cmap,resolved,selector,&glyph);
        if(selector&&status==SfntStatus::unsupported_sequence)status=sfnt_cmap_glyph(cmap,resolved,0,&glyph);
        if(status!=SfntStatus::ok)return status;
        mapped[count++]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),role},source};return SfntStatus::ok;
    };
    if(input.count){const auto c=khmer_category(input[0]);
        if(c>=C::coeng&&c<=C::final){const auto status=map(0x25cc,source_base,0);if(status!=SfntStatus::ok)return status;}}
    for(u32 i=0;i<expanded.count;++i){const u32 scalar=expanded[i].scalar;const auto c=khmer_category(scalar);
        const bool pre=scalar>=0x17c1&&scalar<=0x17c3&&pending==nil;
        u8 role=static_cast<u8>((pre?pre_role:0)|(pre&&i+1<expanded.count&&expanded[i].source==expanded[i+1].source?generated_pre:0));
        if(pending!=nil&&c==C::base){
            if(scalar==0x179a){mapped[pending].glyph.reserved|=ro_first;role|=ro_last;seen_ro=true;}
            else if(seen_ro)cfar=true;
        }
        const u32 selector=i+1<expanded.count&&khmer_category(expanded[i+1].scalar)==C::selector?expanded[i+1].scalar:0;
        const auto status=map(scalar,expanded[i].source,role,selector);if(status!=SfntStatus::ok)return status;
        pending=c==C::coeng?count-1:nil;if(selector)++i;
    }
    if(cfar)for(u32 i=0;i<count;++i)mapped[i].glyph.reserved|=sfnt_khmer_cfar_mask;
    return SfntStatus::ok;
}
struct RunPolicy {Policy cluster;Slice<u32> groups,boundaries;u32 boundary_count;};
static_assert(sizeof(RunPolicy)==64);
SfntStatus run_finish(void* context,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    return finish(&static_cast<RunPolicy*>(context)->cluster,glyphs,zero,work);
}
SfntStatus run_restore(void* context,SfntClusterMap* clusters,const SfntComponentForest& forest,Slice<SfntComponentGlyph> glyphs,u64 work) noexcept {
    auto& p=*static_cast<RunPolicy*>(context);const auto groups=p.groups;
    const auto spend=[&](u64 cost) noexcept {if(cost>work)return false;work-=cost;return true;};
    // Cross-cluster ligatures inherit the last consumed original group. A
    // reordered prefix can still retain the preceding group independently.
    for(u32 i=0;i<glyphs.count;++i){u32 lo=nil,hi=0;u64 cost=0;
        const auto status=sfnt_component_source_bounds(forest,glyphs[i].component,&lo,&hi,work,&cost);
        if(!spend(cost))return SfntStatus::resource_limit;if(status!=SfntStatus::ok)return status;
        if(!spend(1))return SfntStatus::resource_limit;
        const u32 source=lo==nil?glyphs[i].source:hi;if(source>=groups.count)return SfntStatus::invalid_input;
        glyphs[i].source=groups[source];
    }
    if(!spend(static_cast<u64>(p.boundary_count)*3+glyphs.count))return SfntStatus::resource_limit;
    for(u32 i=0;i<p.boundary_count;++i)groups[p.boundaries[i]]=nil;
    for(u32 i=0;i<glyphs.count;++i){auto& first=groups[glyphs[i].source];first=minimum(first,i);}
    u32 next=nil;
    for(u32 i=p.boundary_count;i;){auto& first=groups[p.boundaries[--i]];next=minimum(next,first);first=next;}
    u32 count=0,previous=nil;
    for(u32 i=0;i<p.boundary_count;++i){const u32 source=p.boundaries[i],value=groups[source];
        if(!i||(value!=nil&&value!=previous)){p.boundaries[count++]=source;previous=value;}}
    const u64 cost=static_cast<u64>(sfnt_cluster_map_words(groups.count))*3+count;
    if(!spend(cost))return SfntStatus::resource_limit;
    return sfnt_cluster_map_open(groups.count,{p.boundaries.data,count},clusters->storage,clusters,cost);
}
}
SfntStatus sfnt_khmer_shape_cluster(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    float em,SfntShapeStorage storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,bool rtl,bool defer_output) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((steps.count&&!steps.data)||(storage.positions.count&&!storage.positions.data))return SfntStatus::invalid_input;
    if(steps.count>limits.stage_work)return SfntStatus::resource_limit;
    for(u32 i=0;i<steps.count;++i)if(steps[i].stage>=2||(steps[i].feature_mask&&(steps[i].feature_mask!=sfnt_khmer_cfar_mask||steps[i].stage!=1)))return SfntStatus::invalid_input;
    auto status=sfnt_khmer_cluster_boundaries(input,storage.flags,limits.stage_work);if(status!=SfntStatus::ok)return status;
    for(u32 i=1;i<input.count;++i)if(storage.flags[i])return SfntStatus::unsupported_sequence;
    auto* expanded=reinterpret_cast<SfntKhmerInputScalar*>(storage.positions.data);SfntKhmerInputResult sizes{};
    const u64 slots=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntKhmerInputScalar);
    status=sfnt_khmer_expand_cluster(input,{expanded,static_cast<u32>(minimum<u64>(slots,0xffffffffu))},&sizes,limits.stage_work);if(status!=SfntStatus::ok)return status;
    auto* mapped=expanded?reinterpret_cast<SfntClusterGlyph*>(expanded+sizes.scalar_count):nullptr;u32 count=0;
    static_assert(sizeof(SfntClusterGlyph)==sizeof(SfntKhmerInputScalar));
    status=map_cluster(plan,cmap,input,{expanded,sizes.scalar_count},rtl,{mapped,static_cast<u32>(minimum<u64>(slots-sizes.scalar_count,0xffffffffu))},count);if(status!=SfntStatus::ok)return status;
    status=reorder({mapped,count},limits.stage_work);if(status!=SfntStatus::ok)return status;
    Policy policy{&plan.gdef};policy.units=sizes.source_units;
    status=sfnt_cmap_glyph(cmap,0x20,0,&policy.space);if(status!=SfntStatus::ok)return status;
    for(u32 i=0;i<3;++i){status=sfnt_cmap_glyph(cmap,0x200b+i,0,&policy.zero[i]);if(status!=SfntStatus::ok)return status;}
    const u32 boundary=0;const SfntShapeGlyphPolicy glyph_policy{&policy,finish,restore};
    auto resolved=plan;resolved.retain_zero_positioning=false;resolved.compensate_consecutive_zeroes=true;
    return sfnt_shape_run(resolved,{mapped,count},sizes.source_units,{&boundary,input.count?1u:0u},rtl,em,storage,result,limits,steps,{},glyph_policy,nullptr,nullptr,defer_output);
}
SfntStatus sfnt_khmer_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    float em,SfntShapeStorage storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((steps.count&&!steps.data)||(storage.positions.count&&!storage.positions.data)||
        (storage.character_glyphs.count&&!storage.character_glyphs.data)||(storage.targets.count&&!storage.targets.data)||
        plan.substitution_roots.count)return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*8+steps.count>limits.stage_work)return SfntStatus::resource_limit;
    for(u32 i=0;i<steps.count;++i)if(steps[i].stage>=2||(steps[i].feature_mask&&(steps[i].feature_mask!=sfnt_khmer_cfar_mask||steps[i].stage!=1)))return SfntStatus::invalid_input;
    auto status=sfnt_khmer_cluster_boundaries(input,storage.flags,limits.stage_work);if(status!=SfntStatus::ok)return status;
    auto* expanded=reinterpret_cast<SfntKhmerInputScalar*>(storage.positions.data);SfntKhmerInputResult sizes{};
    const u64 slots=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntKhmerInputScalar);
    status=sfnt_khmer_expand_cluster(input,{expanded,static_cast<u32>(minimum<u64>(slots,0xffffffffu))},&sizes,limits.stage_work);if(status!=SfntStatus::ok)return status;
    if(sizes.source_units>storage.character_glyphs.count)return SfntStatus::output_too_small;
    auto* mapped=expanded?reinterpret_cast<SfntClusterGlyph*>(expanded+sizes.scalar_count):nullptr;
    const u32 capacity=static_cast<u32>(minimum<u64>(slots-sizes.scalar_count,0xffffffffu));
    u32 count=0,units=0,starts=0,pieces=0;u64 reorder_work=limits.stage_work;
    for(u32 first=0;first<input.count;){u32 end=first+1;while(end<input.count&&!storage.flags[end])++end;
        u32 end_units=units;for(u32 i=first;i<end;++i)end_units+=input[i]>0xffff?2u:1u;
        u32 end_pieces=pieces;while(end_pieces<sizes.scalar_count&&expanded[end_pieces].source<end_units)++end_pieces;
        if(starts==storage.targets.count)return SfntStatus::output_too_small;storage.targets[starts++]=units;
        for(u32 i=units;i<end_units;++i)storage.character_glyphs[i]=units;
        u32 amount=0;status=map_cluster(plan,cmap,{input.data+first,end-first},{expanded+pieces,end_pieces-pieces},rtl,{mapped+count,capacity-count},amount,units);
        if(status!=SfntStatus::ok)return status;u64 cost=0;status=reorder({mapped+count,amount},reorder_work,&cost);reorder_work-=cost;
        if(status!=SfntStatus::ok)return status;count+=amount;first=end;units=end_units;pieces=end_pieces;
    }
    RunPolicy policy{{&plan.gdef},{storage.character_glyphs.data,units},storage.targets,starts};policy.cluster.units=units;
    status=sfnt_cmap_glyph(cmap,0x20,0,&policy.cluster.space);if(status!=SfntStatus::ok)return status;
    for(u32 i=0;i<3;++i){status=sfnt_cmap_glyph(cmap,0x200b+i,0,&policy.cluster.zero[i]);if(status!=SfntStatus::ok)return status;}
    auto resolved=plan;resolved.retain_zero_positioning=false;resolved.compensate_consecutive_zeroes=true;
    return sfnt_shape_run(resolved,{mapped,count},units,{storage.targets.data,starts},rtl,em,storage,result,limits,steps,{},
        {&policy,run_finish,run_restore});
}
}
