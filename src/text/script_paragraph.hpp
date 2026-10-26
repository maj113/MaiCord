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
// Shapes one printable segment of a script plan: run(store,plan,text,size,storage,&result,spacing,rtl).
using NativeScriptRun=SfntStatus (*)(const NativeRunPlanStore&,const void* plan,Utf8View,float,const NativeAsciiStorage&,NativeAsciiResult*,float spacing,bool rtl) noexcept;
// The paragraph frontend shared by every script plan; the plan's run handle supplies the metrics.
SfntStatus native_script_paragraph_run(const NativeRunPlanStore& store,const void* plan,NativeRunPlanHandle handle,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,
    float spacing,bool rtl,NativeScriptRun run) noexcept;
// Paragraph frontend for a script whose plan shapes a printable segment through
// Run{}(store,plan,text,size,storage,&result,spacing,rtl).
template<class Plan,class Run> SfntStatus script_paragraph_shape(const NativeRunPlanStore& store,const Plan& plan,NativeRunPlanHandle handle,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,
    float spacing,bool rtl,Run) noexcept {
    return native_script_paragraph_run(store,&plan,handle,text,em,s,boundaries,output,spacing,rtl,
        [](const NativeRunPlanStore& store,const void* plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return Run{}(store,*static_cast<const Plan*>(plan),input,size,storage,result,spacing,rtl);});
}
}
