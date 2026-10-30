#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/devanagari_run_plan.hpp"
namespace maicord {
SfntStatus native_devanagari_run(const NativeRunPlanStore& store,const NativeIndicFormPlan& prepared,Utf8View text,float em,
    const NativeDevanagariRunStorage& storage,NativeAsciiResult* result,const SfntShapeLimits& limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(prepared.indic.profile!=SfntIndicProfile::devanagari)return SfntStatus::invalid_input;
    const auto* parent=native_run_plan_get(store,prepared.indic.run);
    if(!parent||!prepared.indic.cmap)return SfntStatus::invalid_input;
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.indic.positioning_program,{},prepared.indic.positioning());
    return native_devanagari_run(plan,*prepared.indic.cmap,{prepared.forms,128},prepared.indic.substitutions(),text,em,storage,result,limits,trailing_spacing,true,rtl);
}
}
