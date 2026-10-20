#include "run_profile.hpp"
#include "maicord/text/tibetan_run.hpp"
#include "maicord/text/tibetan_shape_run.hpp"
#include "maicord/text/use_plan.hpp"
#include "maicord/text/base.hpp"
#include "compact_run.hpp"
#include "use_shape_internal.hpp"
namespace maicord {
SfntStatus native_tibetan_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,bool independent,
    Arena* state,Arena* scratch,NativeTibetanPlan* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
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
    Utf8View text,float em,SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing,bool admitted) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((text.bytes&&!text.data)||(storage.components.count&&!storage.components.data))return SfntStatus::invalid_input;
    if(!sfnt_trailing_spacing_valid(spacing))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    ConstSlice<u32> scalars{};const auto decoded=decode_run_scalars(text,storage,scalars);if(decoded!=SfntStatus::ok)return decoded;
    SfntStatus status;
    if(admitted)status=sfnt_tibetan_shape_run(plan,cmap,scalars,em,storage,result,steps,limits,rtl);
    else status=use_shape::nominal_run(plan,cmap,scalars,em,storage,result,limits,rtl,true);
    if(status!=SfntStatus::ok||!spacing)return status;
    status=sfnt_trailing_spacing({storage.placements.data,result->glyph_count},
        {storage.flags.data,result->glyph_count},spacing,limits.stage_work,rtl);
    if(status!=SfntStatus::ok)result->glyph_count=result->source_units=result->target_count=0;
    return status;
}
SfntStatus native_tibetan_run(const NativeRunPlanStore& store,const NativeTibetanPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    const auto* parent=native_run_plan_get(store,prepared.run,prepared.independent_letters);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    auto plan=*parent;if(prepared.positioning_program)plan.positioning=prepared.positioning_program;if(prepared.admitted){plan.substitution_roots={};plan.positioning_roots=prepared.positioning();}plan.refresh_substitution_classes=true;
    return native_tibetan_run(plan,*prepared.cmap,prepared.substitutions(),text,em,storage,result,limits,rtl,spacing,prepared.admitted);
}
SfntStatus native_tibetan_run(const NativeRunPlanStore& store,const NativeTibetanPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
        (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
    SfntShapeRunResult shaped{};const auto status=native_tibetan_run(store,prepared,text,em,storage.shaping,&shaped,limits,rtl,spacing);
    return status==SfntStatus::ok?compact_shape_result(storage,shaped,result):status;
}
}
