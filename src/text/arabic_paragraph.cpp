#include "maicord/text/arabic_paragraph.hpp"
#include "script_paragraph.hpp"
#include "unicode_line_geometry.hpp"
namespace maicord {
SfntStatus native_arabic_paragraph_shape(const NativeArabicPlanStore& store,NativeArabicPlanHandle handle,Utf8View input,
    bool independent,bool rtl,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    const auto plan=native_arabic_plan_get(store,handle,independent);if(!plan.shape)return SfntStatus::invalid_input;
    const auto* metrics=native_run_plan_metrics(*store.runs,store.slots[handle.slot-1].run);if(!metrics)return SfntStatus::invalid_input;
    struct Source {const NativeArabicPlanStore& store;NativeArabicPlanHandle handle;const SfntFontLayoutMetrics& metrics;bool independent,rtl;float spacing;} source{store,handle,*metrics,independent,rtl,trailing_spacing};
    const auto shape=[](void* opaque,Utf8View text,float size,const NativeAsciiLineStorage& storage,NativeAsciiLineResult* result) noexcept {
        const auto& c=*static_cast<Source*>(opaque);NativeAsciiLineResult line{};
        if(storage.clusters.count&&!storage.clusters.data)return SfntStatus::invalid_input;
        auto status=native_arabic_run(c.store,c.handle,text,c.independent,c.rtl,size,storage.run,&line.run);
        if(status!=SfntStatus::ok)return status;
        status=sfnt_trailing_spacing({storage.run.placements.data,line.run.glyph_count},{storage.run.shaping.flags.data,line.run.glyph_count},c.spacing,1000000,c.rtl);
        if(status!=SfntStatus::ok)return status;const auto geometry=native_unicode_line_geometry(c.metrics,text,size,storage,&line,true);
        if(geometry==SfntStatus::ok)*result=line;return geometry;
    };
    return native_script_paragraph_shape(*metrics,input,em,s,boundaries,output,{&source,shape});
}
}
