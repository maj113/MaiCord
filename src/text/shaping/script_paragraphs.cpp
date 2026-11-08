#include "script_paragraph.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/shaping/cluster_paragraph.hpp"
// Paragraph entry points of the cluster-shaped scripts, one per run frontend.
namespace maicord {
SfntStatus native_script_paragraph_run(const NativeRunPlanStore& store,const void* plan,NativeRunPlanHandle handle,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,
    float spacing,bool rtl,NativeScriptRun run) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    const auto* metrics=native_run_plan_metrics(store,handle);if(!metrics)return SfntStatus::invalid_input;
    struct Source {const NativeRunPlanStore& store;const void* plan;NativeScriptRun run;const SfntFontLayoutMetrics& metrics;float spacing;bool rtl;} source{store,plan,run,*metrics,spacing,rtl};
    const auto shape=[](void* opaque,Utf8View input,float size,const NativeAsciiLineStorage& storage,NativeAsciiLineResult* result,bool) noexcept {
        const auto& p=*static_cast<Source*>(opaque);NativeAsciiLineResult line{};
        if(storage.clusters.count&&!storage.clusters.data)return SfntStatus::invalid_input;
        const auto status=p.run(p.store,p.plan,input,size,storage.run,&line.run,p.spacing,p.rtl);if(status!=SfntStatus::ok)return status;
        const auto geometry=native_unicode_line_geometry(p.metrics,input,size,storage,&line,NativeLineText::paragraph);
        if(geometry==SfntStatus::ok)*result=line;return geometry;
    };
    return native_script_paragraph_shape(*metrics,text,em,s,boundaries,output,{&source,shape},true);
}
SfntStatus native_cluster_paragraph_shape(NativeClusterScript script,const NativeRunPlanStore& store,const NativeClusterPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    struct Call {NativeClusterScript script;const NativeClusterPlan* plan;} call{script,&plan};
    return native_script_paragraph_run(store,&call,plan.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const void* opaque,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            const auto& call=*static_cast<const Call*>(opaque);
            return native_cluster_run(call.script,store,*call.plan,input,size,storage,result,{},rtl,spacing);});
}
}
