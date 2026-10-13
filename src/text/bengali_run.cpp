#include "maicord/text/bengali_run.hpp"
#include "maicord/text/gujarati_run.hpp"
#include "maicord/text/gurmukhi_run.hpp"
#include "maicord/text/odia_run.hpp"
#include "maicord/text/telugu_run.hpp"
#include "maicord/text/kannada_run.hpp"
#include "maicord/text/malayalam_run.hpp"
#include "indic_cluster.hpp"
#include "maicord/text/detail/indic_form.hpp"
#include "compact_run.hpp"
namespace maicord {
SfntStatus native_bengali_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeBengaliPlan* output,SfntShapeLimits limits) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    return native_indic_plan_prepare(store,run,SfntIndicProfile::bengali,state,scratch,probe,&output->indic,{output->forms,128},limits);
}
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
SfntStatus native_gujarati_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeGujaratiPlan* output,SfntShapeLimits limits) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    return native_indic_plan_prepare(store,run,SfntIndicProfile::gujarati,state,scratch,probe,&output->indic,{output->forms,128},limits);
}
SfntStatus native_bengali_run(const NativeRunPlanStore& store,const NativeBengaliPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::bengali,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_bengali_run(const NativeRunPlanStore& store,const NativeBengaliPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::bengali,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_gujarati_run(const NativeRunPlanStore& store,const NativeGujaratiPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::gujarati,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_gujarati_run(const NativeRunPlanStore& store,const NativeGujaratiPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::gujarati,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_gurmukhi_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeGurmukhiPlan* output,SfntShapeLimits limits) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    return native_indic_plan_prepare(store,run,SfntIndicProfile::gurmukhi,state,scratch,probe,&output->indic,{output->forms,128},limits);
}
SfntStatus native_gurmukhi_run(const NativeRunPlanStore& store,const NativeGurmukhiPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::gurmukhi,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_odia_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeOdiaPlan* output,SfntShapeLimits limits) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    return native_indic_plan_prepare(store,run,SfntIndicProfile::odia,state,scratch,probe,&output->indic,{output->forms,128},limits);
}
SfntStatus native_odia_run(const NativeRunPlanStore& store,const NativeOdiaPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::odia,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_odia_run(const NativeRunPlanStore& store,const NativeOdiaPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::odia,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_telugu_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeTeluguPlan* output,SfntShapeLimits limits) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    return native_indic_plan_prepare(store,run,SfntIndicProfile::telugu,state,scratch,probe,&output->indic,{output->forms,128},limits);
}
SfntStatus native_telugu_run(const NativeRunPlanStore& store,const NativeTeluguPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::telugu,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_telugu_run(const NativeRunPlanStore& store,const NativeTeluguPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::telugu,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_kannada_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeKannadaPlan* output,SfntShapeLimits limits) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    return native_indic_plan_prepare(store,run,SfntIndicProfile::kannada,state,scratch,probe,&output->indic,{output->forms,128},limits);
}
SfntStatus native_malayalam_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,Arena* state,Arena* scratch,
    SfntShapeStorage probe,NativeMalayalamPlan* output,SfntShapeLimits limits) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    return native_indic_plan_prepare(store,run,SfntIndicProfile::malayalam,state,scratch,probe,&output->indic,{output->forms,128},limits);
}
SfntStatus native_malayalam_run(const NativeRunPlanStore& store,const NativeMalayalamPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::malayalam,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_malayalam_run(const NativeRunPlanStore& store,const NativeMalayalamPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::malayalam,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_kannada_run(const NativeRunPlanStore& store,const NativeKannadaPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::kannada,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_kannada_run(const NativeRunPlanStore& store,const NativeKannadaPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::kannada,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
SfntStatus native_gurmukhi_run(const NativeRunPlanStore& store,const NativeGurmukhiPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    return indic_owned_run(SfntIndicProfile::gurmukhi,store,prepared,text,em,storage,result,limits,rtl,spacing);
}
}
