#pragma once
#include "maicord/text/latin_paragraph.hpp"
#include "maicord/core/memory.hpp"
#include "unicode_line_geometry.hpp"
namespace maicord {
// One resolved script/font item. The printable callback and paragraph collector
// share caller storage; no callback state or duplicate glyph array is retained.
struct NativeParagraphLineSource {
    void* context;
    SfntStatus (*shape)(void*,Utf8View,float,const NativeAsciiLineStorage&,NativeAsciiLineResult*) noexcept;
};
SfntStatus native_script_paragraph_shape(const SfntFontLayoutMetrics&,Utf8View,float,
    const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,NativeParagraphLineSource,bool* nominal=nullptr) noexcept;
// Paragraph frontend for a script whose plan shapes a printable segment through
// Run{}(store,plan,text,size,storage,&result,spacing,rtl); the plan's run handle
// supplies the metrics.
template<class Plan,class Run> SfntStatus script_paragraph_shape(const NativeRunPlanStore& store,const Plan& plan,NativeRunPlanHandle handle,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,
    float spacing,bool rtl,Run) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    const auto* metrics=native_run_plan_metrics(store,handle);if(!metrics)return SfntStatus::invalid_input;
    struct Source {const NativeRunPlanStore& store;const Plan& plan;const SfntFontLayoutMetrics& metrics;float spacing;bool rtl;} source{store,plan,*metrics,spacing,rtl};
    const auto shape=[](void* opaque,Utf8View input,float size,const NativeAsciiLineStorage& storage,NativeAsciiLineResult* result) noexcept {
        const auto& p=*static_cast<Source*>(opaque);NativeAsciiLineResult line{};
        if(storage.clusters.count&&!storage.clusters.data)return SfntStatus::invalid_input;
        const auto status=Run{}(p.store,p.plan,input,size,storage.run,&line.run,p.spacing,p.rtl);if(status!=SfntStatus::ok)return status;
        const auto geometry=native_unicode_line_geometry(p.metrics,input,size,storage,&line,true);
        if(geometry==SfntStatus::ok)*result=line;return geometry;
    };
    return native_script_paragraph_shape(*metrics,text,em,s,boundaries,output,{&source,shape});
}
}
