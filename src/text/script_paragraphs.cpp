#include "script_paragraph.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/devanagari_paragraph.hpp"
#include "maicord/text/khmer_paragraph.hpp"
#include "maicord/text/myanmar_paragraph.hpp"
#include "maicord/text/sinhala_paragraph.hpp"
#include "maicord/text/tamil_paragraph.hpp"
#include "maicord/text/tibetan_paragraph.hpp"
// Paragraph entry points of the cluster-shaped scripts, one per run frontend.
namespace maicord {
SfntStatus native_tamil_paragraph_shape(const NativeRunPlanStore& store,const NativeIndicPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    if(output)reset_value(output);if(plan.profile!=SfntIndicProfile::tamil)return SfntStatus::invalid_input;
    return script_paragraph_shape(store,plan,plan.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeIndicPlan& plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return native_tamil_run(store,plan,input,size,storage,result,{},spacing,rtl);});
}
SfntStatus native_sinhala_paragraph_shape(const NativeRunPlanStore& store,const NativeSinhalaPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    return script_paragraph_shape(store,plan,plan.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeSinhalaPlan& plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return native_sinhala_run(store,plan,input,size,storage,result,{},rtl,spacing);});
}
SfntStatus native_khmer_paragraph_shape(const NativeRunPlanStore& store,const NativeKhmerPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    return script_paragraph_shape(store,plan,plan.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeKhmerPlan& plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return native_khmer_run(store,plan,input,size,storage,result,{},rtl,spacing);});
}
SfntStatus native_myanmar_paragraph_shape(const NativeRunPlanStore& store,const NativeMyanmarPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    return script_paragraph_shape(store,plan,plan.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeMyanmarPlan& plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return native_myanmar_run(store,plan,input,size,storage,result,{},rtl,spacing);});
}
SfntStatus native_tibetan_paragraph_shape(const NativeRunPlanStore& store,const NativeTibetanPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    return script_paragraph_shape(store,plan,plan.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeTibetanPlan& plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return native_tibetan_run(store,plan,input,size,storage,result,{},rtl,spacing);});
}
SfntStatus native_devanagari_paragraph_shape(const NativeRunPlanStore& store,const NativeDevanagariPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    return script_paragraph_shape(store,plan,plan.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeDevanagariPlan& plan,Utf8View input,float size,const NativeAsciiStorage& run,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            const auto status=native_devanagari_run(store,plan,input,size,{run.shaping,run.glyphs,run.placements,run.character_glyphs},result,{},spacing,rtl);
            if(status!=SfntStatus::ok)return status;
            for(u32 i=0;i<result->glyph_count;++i)if(!run.glyphs[i])return SfntStatus::unsupported_sequence;
            return SfntStatus::ok;});
}
}
