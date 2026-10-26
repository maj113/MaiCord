#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/tibetan_run.hpp"
#include "maicord/text/tibetan_shape_run.hpp"
#include "maicord/text/use_plan.hpp"
#include "maicord/text/base.hpp"
#include "compact_run.hpp"
#include "use_shape_internal.hpp"
namespace maicord {
SfntStatus native_tibetan_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,bool independent,
    Arena* state,Arena* scratch,NativeTibetanPlan* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    NativePlanSource source{};auto status=native_plan_source(store,run,independent,state,scratch,source);if(status!=SfntStatus::ok)return status;
    const auto& font=*source.font;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeTibetanPlan plan{};plan.run=run;plan.cmap=&font.cmap;plan.independent_letters=independent;
    if(work<2)return SfntStatus::resource_limit;
    SfntLayout layouts[2]{};
    for(u32 kind=0;kind<2;++kind){status=native_layout_open(font,kind,layouts[kind]);
        if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
        if(layouts[kind].script_count>work)return SfntStatus::resource_limit;
        u16 index=0;status=sfnt_layout_find_script(layouts[kind],sfnt_tag('t','i','b','t'),&index);
        // USE admission tests the script record, even without DefaultLangSys.
        if(status==SfntStatus::ok)plan.admitted=true;else if(status!=SfntStatus::missing_table)return status;
    }
    if(!plan.admitted){saved_state.keep=true;*output=plan;return SfntStatus::ok;}
    status=native_plan_tables(source,layouts,*state,*scratch,plan,[&](const SfntLayout& layout,const SfntFeatureSubstitutions& variations,
        Slice<u8> mask,Slice<SfntLookupStep> steps,SfntLookupPlanResult* result) noexcept {
        return sfnt_tibetan_lookup_plan(layout,variations,independent,mask,steps,result,work);},work);
    if(status!=SfntStatus::ok)return status;
    status=native_positioning_prepare(font,*source.parent,plan.positioning(),*state,*scratch,&plan.positioning_program,work);if(status!=SfntStatus::ok)return status;
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
SfntStatus native_tibetan_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntLookupStep> steps,
    const Utf8View& text,float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,bool rtl,float spacing,bool admitted) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    ConstSlice<u32> scalars{};const auto begun=script_run_begin(text,storage,limits,spacing,&scalars);if(begun!=SfntStatus::ok)return begun;
    return script_run_finish(admitted?sfnt_tibetan_shape_run(plan,cmap,scalars,em,storage,result,steps,limits,rtl):
        use_shape::nominal_run(plan,cmap,scalars,em,storage,result,limits,rtl,true),storage,result,limits,spacing,rtl);
}
SfntStatus native_tibetan_run(const NativeRunPlanStore& store,const NativeTibetanPlan& prepared,const Utf8View& text,float em,
    const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    const auto* parent=native_run_plan_get(store,prepared.run,prepared.independent_letters);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.positioning_program,prepared.admitted?ConstSlice<u16>{}:parent->substitution_roots,prepared.admitted?prepared.positioning():parent->positioning_roots);
    return native_tibetan_run(plan,*prepared.cmap,prepared.substitutions(),text,em,storage,result,limits,rtl,spacing,prepared.admitted);
}
SfntStatus native_tibetan_run(const NativeRunPlanStore& store,const NativeTibetanPlan& prepared,const Utf8View& text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,const SfntShapeLimits& limits,bool rtl,float spacing) noexcept {
    return compact_run(storage,result,[&](const SfntShapeStorage& shaping,SfntShapeRunResult* shaped) noexcept {return native_tibetan_run(store,prepared,text,em,shaping,shaped,limits,rtl,spacing);});
}
}
