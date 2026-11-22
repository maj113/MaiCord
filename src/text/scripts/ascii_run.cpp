#include "maicord/text/scripts/ascii_run.hpp"
#include "maicord/core/memory.hpp"
#include "../opentype/nominal_placements.hpp"
#include "../shaping/compact_run.hpp"
#include "maicord/sys/simd.hpp"
#include "maicord/text/opentype/gpos_execute.hpp"
#include "maicord/text/base.hpp"
namespace maicord {
bool kerned_ascii_run(const SfntShapePlan& plan,const NativeNominalPlan& nominal,const NativeKerningPlan& kerning,Utf8View text,float em,
    const NativeAsciiStorage& s,NativeAsciiResult* out,const SfntShapeLimits& limits) noexcept {
    const u32 n=text.bytes;const auto& shaping=s.shaping;
    if(!n||!nominal.ascii_advances||n>shaping.glyphs.count||n>shaping.positions.count||n>shaping.flags.count||
       n>s.glyphs.count||n>s.placements.count||n>s.character_glyphs.count)return false;
    // Packed records in the rich glyph storage. The compact glyphs, written last, can share its prefix.
    auto* const records=reinterpret_cast<SfntGsubGlyph*>(shaping.glyphs.data);
    for(u32 i=0;i<n;++i){const u32 offset=static_cast<u8>(text.data[i])-32u;
        if(offset>=nominal.ascii_count||!((kerning.ready[offset/64]>>(offset%64))&1))return false;
        records[i]={static_cast<u16>(nominal.ascii_first+offset),((kerning.base[offset/64]>>(offset%64))&1)?SfntGlyphClass::base:SfntGlyphClass::unassigned,0};
        shaping.positions[i]={static_cast<double>(nominal.ascii_advances[offset]),0,0};shaping.flags[i]=sfnt_position_cluster_start;}
    const SfntGlyphView view{records,nullptr,n,0,sizeof(SfntGsubGlyph)};
    u64 work=limits.lookup_operations;
    for(u32 r=0;r<kerning.roots.count;++r){const auto& lookup=*sfnt_gpos_lookup(*plan.positioning,kerning.roots[r]);
        if(!work)return false;--work;SfntPositionExecution execution{};
        const auto status=sfnt_position_execute_lookup({lookup.tables.basic,lookup.filter},plan.positioning->gdef,view,{shaping.positions.data,n},0,n,false,
            plan.position_instance,&execution,work,limits.match_work,SfntPositionApplication::scan,&lookup.bounds);
        work-=execution.operations;if(status!=SfntStatus::ok)return false;}
    if(plan.legacy_kern.count&&sfnt_legacy_kern_apply(plan.legacy_kern,view,{shaping.flags.data,n},{shaping.positions.data,n},false,&work)!=SfntStatus::ok)return false;
    if(sfnt_position_finalize({shaping.positions.data,n},{shaping.flags.data,n},em,plan.units_per_em,{s.placements.data,n},limits.stage_work,
        plan.retain_zero_positioning)!=SfntStatus::ok)return false;
    for(u32 i=0;i<n;++i){s.glyphs[i]=static_cast<u16>(nominal.ascii_first+static_cast<u8>(text.data[i])-32u);s.character_glyphs[i]=i;}
    *out={n,n,0,1,limits.lookup_operations-work};return true;
}
SfntStatus native_ascii_run(const NativeRunPlanStore& store,NativeRunPlanHandle handle,
    Utf8View text,bool independent,float em,const NativeAsciiStorage& s,NativeAsciiResult* result,SfntShapeLimits limits) noexcept {
    const auto mapped=s.mapped;const auto glyphs=s.glyphs;const auto placements=s.placements;const auto sources=s.character_glyphs;
    reset_value(result);u32 em_bits=0;copy_bytes(&em_bits,&em,4);
    const auto* plan=native_run_plan_get(store,handle,independent);
    if(!plan||!store.instances->fonts||(text.bytes&&!text.data)||!em_bits||em_bits>=0x7f800000u||em>65536||
       !filled(mapped)||!filled(glyphs)||!filled(placements)||!filled(sources))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    if(text.bytes>mapped.count||text.bytes>sources.count)return SfntStatus::output_too_small;
    const auto nominal=native_run_plan_nominal(store,handle);if(!nominal.glyph_limit)return SfntStatus::invalid_input;
    // Any miss in the common case falls through to the general path, which also reports the errors.
    if(text.bytes&&!independent&&nominal.ascii_advances&&text.bytes<=glyphs.count&&text.bytes<=placements.count&&
       nominal_ascii_prefix(text,nominal,_mm_div_ss(_mm_set_ss(em),_mm_set_ss(static_cast<float>(plan->units_per_em))),glyphs.data,placements.data,sources.data)==text.bytes){
        *result={text.bytes,text.bytes,1,0,0};return SfntStatus::ok;}
    if(text.bytes&&!independent){const auto* kerning=native_run_plan_kerning(store,handle);
        if(kerning&&kerned_ascii_run(*plan,nominal,*kerning,text,em,s,result,limits))return SfntStatus::ok;}
    const auto* instance=native_font_instance_get(*store.instances,store.slots[handle.slot-1].instance);
    if(!instance)return SfntStatus::invalid_input;
    const auto* font=native_font_get(*store.instances->fonts,instance->face);if(!font)return SfntStatus::invalid_input;
    bool nominal_run=!independent;
    for(u32 i=0;i<text.bytes;++i){u16 glyph=0;const u32 offset=static_cast<u8>(text.data[i])-32u;
        if(offset>94)return SfntStatus::unsupported_sequence;
        if(offset<nominal.ascii_count)glyph=static_cast<u16>(nominal.ascii_first+offset);
        else{const auto status=sfnt_cmap_glyph(font->cmap,static_cast<u8>(text.data[i]),0,&glyph);if(status!=SfntStatus::ok)return status;}
        if(!glyph)return SfntStatus::unsupported_sequence;
        mapped[i]={{glyph,SfntGlyphClass::unassigned,0},i};
        nominal_run=nominal_run&&(!nominal.participation.count||!(nominal.participation[glyph/64]&(1ull<<(glyph%64))));}
    if(!text.bytes)return SfntStatus::ok;
    const __m128 scale=_mm_div_ss(_mm_set_ss(em),_mm_set_ss(static_cast<float>(plan->units_per_em)));
    if(nominal_run){
        if(text.bytes>glyphs.count||text.bytes>placements.count)return SfntStatus::output_too_small;
        if(plan->cache_advances){
            const auto status=nominal_placements(*plan,{mapped.data,text.bytes},glyphs,placements,s.shaping.targets,scale);if(status!=SfntStatus::ok)return status;
            for(u32 i=0;i<text.bytes;++i)sources[i]=i;
        }else for(u32 i=0;i<text.bytes;++i){double advance=0;const u16 glyph=mapped[i].glyph.glyph;const auto status=nominal_advance(nominal,*plan,glyph,&advance);if(status!=SfntStatus::ok)return status;
            const float scaled=_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(static_cast<float>(advance)),scale));
            glyphs[i]=glyph;placements[i]={scaled,0,0};sources[i]=i;}
        *result={text.bytes,text.bytes,1,0,0};return SfntStatus::ok;
    }
    SfntGlyphClassMemo classes;
    for(u32 i=0;i<text.bytes;++i)mapped[i].glyph.glyph_class=sfnt_gdef_glyph_class(plan->gdef,mapped[i].glyph.glyph,classes);
    SfntShapeRunResult shaped{};
    const auto status=sfnt_shape_run(*plan,{mapped.data,text.bytes},text.bytes,{},false,em,s.shaping,&shaped,limits);
    result->lookup_operations=shaped.lookup_operations;if(status!=SfntStatus::ok)return status;
    return compact_publish(s,shaped.glyph_count,text.bytes,shaped.lookup_operations,result);
}
}
