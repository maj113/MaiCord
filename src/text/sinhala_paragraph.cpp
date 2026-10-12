#include "maicord/text/sinhala_paragraph.hpp"
#include "script_paragraph.hpp"
#include "unicode_line_geometry.hpp"
namespace maicord {
SfntStatus native_sinhala_paragraph_shape(const NativeRunPlanStore& store,const NativeSinhalaPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    const auto* metrics=native_run_plan_metrics(store,plan.run);if(!metrics)return SfntStatus::invalid_input;
    struct Source {const NativeRunPlanStore& store;const NativeSinhalaPlan& plan;const SfntFontLayoutMetrics& metrics;float spacing;bool rtl;} source{store,plan,*metrics,trailing_spacing,rtl};
    const auto shape=[](void* opaque,Utf8View input,float size,const NativeAsciiLineStorage& storage,NativeAsciiLineResult* result) noexcept {
        const auto& p=*static_cast<Source*>(opaque);NativeAsciiLineResult line{};
        if(storage.clusters.count&&!storage.clusters.data)return SfntStatus::invalid_input;
        const auto status=native_sinhala_run(p.store,p.plan,input,size,storage.run,&line.run,{},p.rtl,p.spacing);if(status!=SfntStatus::ok)return status;
        const auto geometry=native_unicode_line_geometry(p.metrics,input,size,storage,&line,true);
        if(geometry==SfntStatus::ok)*result=line;return geometry;
    };
    return native_script_paragraph_shape(*metrics,text,em,s,boundaries,output,{&source,shape});
}
}
