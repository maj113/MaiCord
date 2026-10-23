#include "run_profile.hpp"
#include "maicord/text/tamil_run.hpp"
#include "maicord/text/tamil_shape_run.hpp"
#include "maicord/text/base.hpp"
#include "compact_run.hpp"
namespace maicord {
SfntStatus native_tamil_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntLookupStep> steps,
    Utf8View text,float em,SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((text.bytes&&!text.data)||(storage.components.count&&!storage.components.data))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    ConstSlice<u32> scalars{};const auto decoded=decode_run_scalars(text,storage,scalars);if(decoded!=SfntStatus::ok)return decoded;
    return sfnt_tamil_shape_run(plan,cmap,scalars,em,storage,result,steps,limits,trailing_spacing,rtl);
}
SfntStatus native_tamil_run(const NativeRunPlanStore& store,const NativeIndicPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if(prepared.profile!=SfntIndicProfile::tamil)return SfntStatus::invalid_input;
    const auto* parent=native_run_plan_get(store,prepared.run);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.positioning_program,{},prepared.positioning());
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
