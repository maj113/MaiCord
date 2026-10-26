#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/tamil_run.hpp"
#include "maicord/text/tamil_shape_run.hpp"
#include "maicord/text/base.hpp"
#include "compact_run.hpp"
namespace maicord {
SfntStatus native_tamil_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntLookupStep> steps,
    Utf8View text,float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if((text.bytes&&!text.data)||(storage.components.count&&!storage.components.data))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    ConstSlice<u32> scalars{};const auto decoded=decode_run_scalars(text,storage,scalars);if(decoded!=SfntStatus::ok)return decoded;
    return sfnt_tamil_shape_run(plan,cmap,scalars,em,storage,result,steps,limits,trailing_spacing,rtl);
}
SfntStatus native_tamil_run(const NativeRunPlanStore& store,const NativeIndicPlan& prepared,Utf8View text,float em,
    const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(prepared.profile!=SfntIndicProfile::tamil)return SfntStatus::invalid_input;
    const auto* parent=native_run_plan_get(store,prepared.run);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.positioning_program,{},prepared.positioning());
    return native_tamil_run(plan,*prepared.cmap,prepared.substitutions(),text,em,storage,result,limits,trailing_spacing,rtl);
}
SfntStatus native_tamil_run(const NativeRunPlanStore& store,const NativeIndicPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,const SfntShapeLimits& limits,float trailing_spacing,bool rtl) noexcept {
    return compact_run(storage,result,[&](const SfntShapeStorage& shaping,SfntShapeRunResult* shaped) noexcept {return native_tamil_run(store,prepared,text,em,shaping,shaped,limits,trailing_spacing,rtl);});
}
}
