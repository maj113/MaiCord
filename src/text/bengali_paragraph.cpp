#include "script_paragraph.hpp"
#include "maicord/text/detail/indic_form.hpp"
namespace maicord {
SfntStatus indic_form_paragraph(SfntIndicProfile profile,const NativeRunPlanStore& store,const NativeIndicFormPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!indic_form_profile(profile)||plan.indic.profile!=profile)return SfntStatus::invalid_input;
    return script_paragraph_shape(store,plan,plan.indic.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeIndicFormPlan& plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return indic_owned_run(plan.indic.profile,store,plan,input,size,storage,result,{},rtl,spacing);});
}
}
