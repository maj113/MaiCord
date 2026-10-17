#include "indic_cluster.hpp"
#include "maicord/text/detail/indic_form.hpp"
#include "compact_run.hpp"
namespace maicord {
SfntStatus indic_owned_run(SfntIndicProfile profile,const NativeRunPlanStore& store,const NativeIndicFormPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float trailing_spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if(!indic_form_profile(profile)||prepared.indic.profile!=profile)return SfntStatus::invalid_input;
    const auto* parent=native_run_plan_get(store,prepared.indic.run);if(!parent||!prepared.indic.cmap)return SfntStatus::invalid_input;
    auto plan=*parent;if(prepared.indic.positioning_program)plan.positioning=prepared.indic.positioning_program;plan.substitution_roots={};plan.positioning_roots=prepared.indic.positioning();plan.refresh_substitution_classes=true;
    return indic_utf8_run(profile,plan,*prepared.indic.cmap,text,{prepared.forms,128},em,storage,result,prepared.indic.substitutions(),limits,rtl,trailing_spacing);
}
SfntStatus indic_owned_run(SfntIndicProfile profile,const NativeRunPlanStore& store,const NativeIndicFormPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float trailing_spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
       (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
    SfntShapeRunResult shaped{};const auto status=indic_owned_run(profile,store,prepared,text,em,storage.shaping,&shaped,limits,rtl,trailing_spacing);
    return status==SfntStatus::ok?compact_shape_result(storage,shaped,result):status;
}
}
