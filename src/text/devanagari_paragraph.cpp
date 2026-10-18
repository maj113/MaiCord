#include "maicord/text/devanagari_paragraph.hpp"
#include "script_paragraph.hpp"
namespace maicord {
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
