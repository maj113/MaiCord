#include "maicord/text/myanmar_paragraph.hpp"
#include "script_paragraph.hpp"
namespace maicord {
SfntStatus native_myanmar_paragraph_shape(const NativeRunPlanStore& store,const NativeMyanmarPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    return script_paragraph_shape(store,plan,plan.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeMyanmarPlan& plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return native_myanmar_run(store,plan,input,size,storage,result,{},rtl,spacing);});
}
}
