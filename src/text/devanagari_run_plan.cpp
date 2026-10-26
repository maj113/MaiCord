#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/devanagari_run_plan.hpp"
#include "maicord/text/indic_run_plan.hpp"
namespace maicord {
SfntStatus native_devanagari_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,
    Arena* state,Arena* scratch,const SfntShapeStorage& probe,NativeDevanagariPlan* output,const SfntShapeLimits& limits) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);NativeIndicPlan prepared{};
    const auto status=native_indic_plan_prepare(store,run,SfntIndicProfile::devanagari,state,scratch,probe,&prepared,{output->forms,128},limits);
    if(status!=SfntStatus::ok)return status;
    output->run=prepared.run;output->cmap=prepared.cmap;output->substitutions=prepared.substitutions();output->positioning=prepared.positioning();output->positioning_program=prepared.positioning_program;
    return SfntStatus::ok;
}
SfntStatus native_devanagari_run(const NativeRunPlanStore& store,const NativeDevanagariPlan& prepared,Utf8View text,float em,
    const NativeDevanagariRunStorage& storage,NativeAsciiResult* result,const SfntShapeLimits& limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);const auto* parent=native_run_plan_get(store,prepared.run);
    if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.positioning_program,{},prepared.positioning);
    return native_devanagari_run(plan,*prepared.cmap,{prepared.forms,128},prepared.substitutions,text,em,storage,result,limits,trailing_spacing,true,rtl);
}
}
