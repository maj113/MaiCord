#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/khmer_run.hpp"
#include "maicord/text/khmer_shape_run.hpp"
#include "maicord/text/khmer_plan.hpp"
#include "maicord/text/latin_run.hpp"
#include "maicord/text/base.hpp"
#include "compact_run.hpp"
#include "lookup_plan_finish.hpp"
namespace maicord {
SfntStatus native_khmer_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,bool independent,
    Arena* state,Arena* scratch,NativeKhmerPlan* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    NativePlanSource source{};auto status=native_plan_source(store,run,independent,state,scratch,source);if(status!=SfntStatus::ok)return status;
    const auto& parent=*source.parent;const auto& font=*source.font;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeKhmerPlan plan{};plan.run=run;plan.cmap=&font.cmap;plan.independent_letters=independent;
    SfntLayout layouts[2]{};bool script_found=false;
    if(work<3)return SfntStatus::resource_limit;
    for(u32 kind=0;kind<2;++kind){status=native_layout_open(font,kind,layouts[kind]);
        if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
        if(layouts[kind].script_count>work)return SfntStatus::resource_limit;
        u16 index=0;status=sfnt_layout_find_script(layouts[kind],sfnt_tag('k','h','m','r'),&index);
        if(status==SfntStatus::ok)script_found=true;else if(status!=SfntStatus::missing_table)return status;
    }
    ConstSlice<u8> gdef{};status=sfnt_table(font.geometry.face,sfnt_tag('G','D','E','F'),&gdef);
    if(status!=SfntStatus::ok&&status!=SfntStatus::missing_table)return status;
    plan.admitted=status==SfntStatus::ok&&layouts[0].table.data&&layouts[1].table.data&&script_found;
    if(!plan.admitted){plan.fallback_roots=parent.substitution_roots.data;plan.substitution_count=parent.substitution_roots.count;
        plan.positioning_roots=parent.positioning_roots.data;plan.positioning_count=static_cast<u16>(parent.positioning_roots.count);}
    for(u32 kind=0;kind<2;++kind){const auto& layout=layouts[kind];if(!layout.table.data||(!plan.admitted&&(!independent||kind)))continue;
        ArenaScope temporary{scratch,arena_mark(scratch)};SfntFeatureSubstitutions variations{};
        status=native_layout_variations(*source.instance,layout,*scratch,variations,work);if(status!=SfntStatus::ok)return status;
        if(!plan.admitted){SfntLookupPlanResult result{};auto mask=take<u64>(scratch,(layout.lookup_count+63u)/64);if(!filled(mask))return SfntStatus::output_too_small;
            status=sfnt_khmer_fallback_substitutions(layout,variations,mask,{},&result,work);
            if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
            auto roots=take<u16>(state,result.lookup_count);if(!filled(roots))return SfntStatus::resource_limit;
            status=finish_lookup_plan({mask.data,mask.count},roots,result.operations,work);if(status!=SfntStatus::ok)return status;
            plan.fallback_roots=roots.data;plan.substitution_count=roots.count;continue;}
        auto mask=take<u8>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        ConstSlice<SfntLookupStep> steps{};ConstSlice<u16> roots{};
        status=native_plan_steps(kind,*state,*scratch,[&](Slice<SfntLookupStep> out,SfntLookupPlanResult* result) noexcept {
            return sfnt_khmer_lookup_plan(layout,variations,mask,out,result,work);},steps,roots);
        if(status!=SfntStatus::ok)return status;
        if(!kind){plan.substitution_steps=steps.data;plan.substitution_count=steps.count;}
        else{plan.positioning_roots=roots.data;plan.positioning_count=static_cast<u16>(roots.count);}
    }
    status=native_positioning_prepare(font,parent,plan.positioning(),*state,*scratch,&plan.positioning_program,work);if(status!=SfntStatus::ok)return status;
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
SfntStatus native_khmer_run(const NativeRunPlanStore& store,const NativeKhmerPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    const auto* parent=native_run_plan_get(store,prepared.run,prepared.independent_letters);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    if((text.bytes&&!text.data)||(storage.components.count&&!storage.components.data))return SfntStatus::invalid_input;
    if(!sfnt_trailing_spacing_valid(spacing))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    SfntStatus status;
    if(prepared.admitted){ConstSlice<u32> scalars{};const auto decoded=decode_run_scalars(text,storage,scalars);if(decoded!=SfntStatus::ok)return decoded;
        SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.positioning_program,{},prepared.positioning());
        status=sfnt_khmer_shape_run(plan,*prepared.cmap,scalars,em,storage,result,prepared.substitutions(),limits,rtl);
    }else status=native_khmer_fallback_shape_run(store,prepared.run,text,prepared.independent_letters,rtl,em,storage,result,
        {prepared.fallback_roots,prepared.substitution_count},prepared.positioning(),limits);
    if(status!=SfntStatus::ok||!spacing)return status;
    status=sfnt_trailing_spacing({storage.placements.data,result->glyph_count},{storage.flags.data,result->glyph_count},spacing,limits.stage_work,rtl);
    if(status!=SfntStatus::ok)result->glyph_count=result->source_units=result->target_count=0;return status;
}
SfntStatus native_khmer_run(const NativeRunPlanStore& store,const NativeKhmerPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
        (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
    SfntShapeRunResult shaped{};const auto status=native_khmer_run(store,prepared,text,em,storage.shaping,&shaped,limits,rtl,spacing);
    return status==SfntStatus::ok?compact_shape_result(storage,shaped,result):status;
}
}
