#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/shaping/cluster_run.hpp"
#include "maicord/text/scripts/syllabic_shape.hpp"
#include "maicord/text/base.hpp"
#include "compact_run.hpp"
namespace maicord {
static_assert(static_cast<u8>(NativeClusterScript::sinhala)==static_cast<u8>(SfntSyllabicScript::sinhala)&&
    static_cast<u8>(NativeClusterScript::tibetan)==static_cast<u8>(SfntSyllabicScript::tibetan));
namespace {
// HarfBuzz's default shaper for the font (hb_ot_shaper_categorize): its GSUB selected DFLT or latn, or mymr for
// Myanmar, rather than the script's own tags. Khmer always keeps its shaper.
bool default_shaper(NativeClusterScript script,u32 selected) noexcept {
    if(script==NativeClusterScript::khmer)return false;
    return selected==sfnt_tag('D','F','L','T')||selected==sfnt_tag('l','a','t','n')||
        (script==NativeClusterScript::myanmar&&selected==sfnt_tag('m','y','m','r'));
}
}
SfntStatus native_cluster_plan_prepare(NativeClusterScript script,const NativeRunPlanStore& store,NativeRunPlanHandle run,bool independent,
    Arena* state,Arena* scratch,NativeClusterPlan* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(static_cast<u8>(script)>3)return SfntStatus::invalid_input;
    NativePlanSource source{};auto status=native_plan_source(store,run,independent,state,scratch,source);if(status!=SfntStatus::ok)return status;
    const auto& font=*source.font;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeClusterPlan plan{};plan.run=run;plan.cmap=&font.cmap;plan.independent_letters=independent;
    const auto syllabic=static_cast<SfntSyllabicScript>(script);
    for(u32 kind=0;kind<2;++kind){ArenaScope temporary{scratch,arena_mark(scratch)};SfntLayout layout{};
        status=native_layout_open(font,kind,layout);if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
        SfntFeatureSubstitutions variations{};status=native_layout_variations(*source.instance,layout,*scratch,variations,work);if(status!=SfntStatus::ok)return status;
        auto mask=take<u16>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        ConstSlice<SfntLookupStep> steps{};ConstSlice<u16> roots{};Slice<u64> per_syllable{};SfntLookupPlanResult found{};
        status=native_plan_steps(kind,*state,*scratch,[&](Slice<SfntLookupStep> out,SfntLookupPlanResult* result) noexcept {
            if(!kind&&out.count){per_syllable=take<u64>(state,(out.count+63)/64);if(!filled(per_syllable))return SfntStatus::resource_limit;}
            return sfnt_syllabic_lookup_plan(layout,variations,syllabic,independent,mask,out,result,work,per_syllable);},steps,roots,&found);
        if(status!=SfntStatus::ok)return status;
        if(kind){plan.positioning_roots=roots.data;plan.positioning_count=static_cast<u16>(roots.count);continue;}
        if(default_shaper(script,found.script_tag)){
            // The default shaper is the Common alphabetic profile (DFLT, then latn).
            auto* fallback=arena_allocate<NativeAlphabeticPlan>(state);if(!fallback)return SfntStatus::resource_limit;
            status=native_alphabetic_plan_prepare(store,run,0,independent,state,scratch,fallback,work);if(status!=SfntStatus::ok)return status;
            plan.fallback=fallback;saved_state.keep=true;*output=plan;return SfntStatus::ok;
        }
        plan.substitution_steps=steps.data;plan.substitution_count=steps.count;plan.per_syllable_steps=per_syllable.data;
        ConstSlice<u64> manual{};status=native_manual_joiners(layout,variations,sfnt_syllabic_manual_joiners(syllabic),*state,manual,work);
        if(status!=SfntStatus::ok)return status;plan.substitution_manual=manual.data;
    }
    status=native_positioning_prepare(font,*source.parent,plan.positioning(),*state,*scratch,&plan.positioning_program,work);if(status!=SfntStatus::ok)return status;
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
SfntStatus native_cluster_run(NativeClusterScript script,const NativeRunPlanStore& store,const NativeClusterPlan& prepared,const Utf8View& text,float em,
    const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    const auto* parent=native_run_plan_get(store,prepared.run,prepared.independent_letters);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    if(prepared.fallback)return SfntStatus::unsupported_sequence;
    const auto begun=script_run_begin(text,storage,limits,spacing,nullptr);if(begun!=SfntStatus::ok)return begun;
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.positioning_program,{},prepared.positioning());
    plan.substitution_manual=prepared.substitution_manual&&parent->substitutions?
        ConstSlice<u64>{prepared.substitution_manual,(2u*parent->substitutions->lookups.count+63u)/64u}:ConstSlice<u64>{};
    const auto status=sfnt_syllabic_run(static_cast<SfntSyllabicScript>(script),plan,*prepared.cmap,text,em,storage,result,prepared.substitutions(),
        prepared.per_syllable(),limits,rtl);
    return script_run_finish(status,storage,result,limits,spacing,rtl);
}
SfntStatus native_cluster_run(NativeClusterScript script,const NativeRunPlanStore& store,const NativeClusterPlan& prepared,const Utf8View& text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,const SfntShapeLimits& limits,bool rtl,float spacing) noexcept {
    if(prepared.fallback){
        // HarfBuzz's default shaper; tracking applies as in the alphabetic frontend's callers. A character the font
        // lacks rejects the run so the caller can fall back to another font.
        if(!result)return SfntStatus::invalid_input;reset_value(result);
        if(!native_run_plan_get(store,prepared.run,prepared.independent_letters))return SfntStatus::invalid_input;
        auto status=native_alphabetic_run(store,*prepared.fallback,text,em,storage,result,limits,true,false,rtl,false);
        if(status==SfntStatus::ok&&spacing)status=sfnt_trailing_spacing({storage.placements.data,result->glyph_count},
            result->nominal_ranges?ConstSlice<u8>{}:ConstSlice<u8>{storage.shaping.flags.data,result->glyph_count},spacing,limits.stage_work,rtl);
        if(status!=SfntStatus::ok)reset_value(result);
        return status;
    }
    return compact_run(storage,result,[&](const SfntShapeStorage& shaping,SfntShapeRunResult* shaped) noexcept {return native_cluster_run(script,store,prepared,text,em,shaping,shaped,limits,rtl,spacing);});
}
}
