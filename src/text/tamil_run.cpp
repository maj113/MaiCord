#include "maicord/text/tamil_run.hpp"
#include "maicord/text/tamil_shape_run.hpp"
#include "maicord/core/utf.hpp"
#include "compact_run.hpp"
namespace maicord {
SfntStatus native_tamil_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntLookupStep> steps,
    Utf8View text,float em,SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((text.bytes&&!text.data)||(storage.components.count&&!storage.components.data))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    auto* scalars=reinterpret_cast<u32*>(storage.components.data);
    const u64 capacity=static_cast<u64>(storage.components.count)*sizeof(SfntComponentNode)/sizeof(u32);u32 count=0;
    for(u32 byte=0;byte<text.bytes;){const auto decoded=utf8_decode_one(text.data+byte,text.bytes-byte);
        if(!decoded.valid)return SfntStatus::invalid_input;if(count>=capacity)return SfntStatus::output_too_small;
        scalars[count++]=decoded.codepoint;byte+=decoded.width;}
    return sfnt_tamil_shape_run(plan,cmap,{scalars,count},em,storage,result,steps,limits,trailing_spacing,rtl);
}
SfntStatus native_tamil_run(const NativeRunPlanStore& store,const NativeIndicPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if(prepared.profile!=SfntIndicProfile::tamil)return SfntStatus::invalid_input;
    const auto* parent=native_run_plan_get(store,prepared.run);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    auto plan=*parent;if(prepared.positioning_program)plan.positioning=prepared.positioning_program;plan.substitution_roots={};plan.positioning_roots=prepared.positioning();plan.refresh_substitution_classes=true;
    return native_tamil_run(plan,*prepared.cmap,prepared.substitutions(),text,em,storage,result,limits,trailing_spacing,rtl);
}
SfntStatus native_tamil_run(const NativeRunPlanStore& store,const NativeIndicPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
        (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
    SfntShapeRunResult shaped{};const auto status=native_tamil_run(store,prepared,text,em,storage.shaping,&shaped,limits,trailing_spacing,rtl);
    if(status!=SfntStatus::ok)return status;
    return compact_shape_result(storage,shaped,result);
}
}
