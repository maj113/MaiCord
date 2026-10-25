#include "maicord/core/memory.hpp"
#include "maicord/text/khmer_input.hpp"
#include "maicord/text/khmer_plan.hpp"
#include "khmer_character.hpp"
#include "use_shape_internal.hpp"
namespace maicord {namespace {
using C=KhmerCategory;
constexpr u32 nil=0xffffffffu;
constexpr u8 pre_role=4,ro_first=8,ro_last=16,generated_pre=64;
SfntStatus reorder(Slice<SfntClusterGlyph> glyphs,u64 available,u64* spent) noexcept {
    u64 used=0;const auto spend=[&](u64 cost) noexcept {if(cost>available-used)return false;used+=cost;return true;};
    struct Report {u64& used;u64* out;~Report() noexcept {*out=used;}} report{used,spent};
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
SfntStatus map_cluster(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    ConstSlice<SfntKhmerInputScalar> expanded,bool rtl,Slice<SfntClusterGlyph> mapped,u32& count,u32 source_base) noexcept {
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
}
SfntStatus sfnt_khmer_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    float em,SfntShapeStorage storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
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
    use_shape::GroupRun policy{&plan.gdef,0,{},{storage.character_glyphs.data,units},storage.targets,starts};
    status=sfnt_cmap_glyph(cmap,0x20,0,&policy.space);if(status!=SfntStatus::ok)return status;
    // Postprocessing treats the font's ZWSP/ZWNJ/ZWJ glyphs as invisible.
    for(u32 i=0;i<3;++i){status=sfnt_cmap_glyph(cmap,0x200b+i,0,&policy.invisible[i]);if(status!=SfntStatus::ok)return status;}
    policy.invisible[3]=policy.invisible[0];
    auto resolved=plan;resolved.retain_zero_positioning=false;resolved.compensate_consecutive_zeroes=true;
    return sfnt_shape_run(resolved,{mapped,count},units,{storage.targets.data,starts},rtl,em,storage,result,limits,steps,{},
        {&policy,use_shape::finish_group_run,use_shape::restore_group_run});
}
}
