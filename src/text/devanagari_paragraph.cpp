#include "maicord/text/devanagari_paragraph.hpp"
#include "script_paragraph.hpp"
#include "unicode_line_geometry.hpp"
namespace maicord {
SfntStatus native_devanagari_paragraph_shape(const NativeRunPlanStore& store,const NativeDevanagariPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};const auto* metrics=native_run_plan_metrics(store,plan.run);
    if(!metrics)return SfntStatus::invalid_input;
    struct Source {const NativeRunPlanStore& store;const NativeDevanagariPlan& plan;const SfntFontLayoutMetrics& metrics;float spacing;bool rtl;} source{store,plan,*metrics,trailing_spacing,rtl};
    const auto shape=[](void* opaque,Utf8View input,float size,const NativeAsciiLineStorage& storage,NativeAsciiLineResult* result) noexcept {
        const auto& p=*static_cast<Source*>(opaque);NativeAsciiLineResult line{};
        if(storage.clusters.count&&!storage.clusters.data)return SfntStatus::invalid_input;
        const auto& run=storage.run;const auto status=native_devanagari_run(p.store,p.plan,input,size,
            {run.shaping,run.glyphs,run.placements,run.character_glyphs},&line.run,{},p.spacing,p.rtl);if(status!=SfntStatus::ok)return status;
        for(u32 i=0;i<line.run.glyph_count;++i)if(!run.glyphs[i])return SfntStatus::unsupported_sequence;
        const auto geometry=native_unicode_line_geometry(p.metrics,input,size,storage,&line,true);
        if(geometry==SfntStatus::ok)*result=line;return geometry;
    };
    return native_script_paragraph_shape(*metrics,text,em,s,boundaries,output,{&source,shape});
}
}
