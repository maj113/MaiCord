#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/arabic_run_plan.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {namespace {
// The fallback flag occupies existing slice padding; retained plan size is unchanged.
struct StoredSteps {const SfntLookupStep* data;u32 count;bool generic=false;};
static_assert(sizeof(StoredSteps)==sizeof(ConstSlice<SfntLookupStep>));
struct PlanData {SfntShapePlan shape;StoredSteps steps[2];const SfntCmap* cmap;};
NativeFontResult failure(SfntStatus status) noexcept {return {NativeFontError::font,status,0};}
NativeArabicPlanSlot* slot(const NativeArabicPlanStore& store,NativeArabicPlanHandle h) noexcept {
    if(!store.runs || !store.slots.data || !h.slot || h.slot>store.slots.count || !h.generation)return nullptr;
    auto& value=store.slots[h.slot-1];return value.references && value.generation==h.generation?&value:nullptr;
}
SfntStatus prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,PlanData& data,Arena& state,Arena& scratch,u32 work) noexcept {
    NativePlanSource source{};auto status=native_plan_source(store,run,false,&state,&scratch,source);if(status!=SfntStatus::ok)return status;
    const auto* base=source.parent;const auto* font=source.font;
    data.cmap=&font->cmap;data.shape=*base;reset_value(&data.shape.substitution_roots);reset_value(&data.shape.positioning_roots);
    for(u32 kind=0;kind<2;++kind){SfntLayout layout{};status=native_layout_open(*font,kind,layout);
        if(status==SfntStatus::missing_table){if(!kind)return SfntStatus::unsupported_sequence;continue;}if(status!=SfntStatus::ok)return status;
        const auto mark=arena_mark(&scratch);SfntFeatureSubstitutions variations{};
        status=native_layout_variations(*source.instance,layout,scratch,variations,work);if(status!=SfntStatus::ok)return status;
        auto mask=take<u8>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        const u32 modes=kind?1u:2u; // Arabic positioning is identical in both modes.
        for(u32 mode=0;mode<modes;++mode){SfntLookupPlanResult result{};
            status=sfnt_arabic_lookup_plan(layout,variations,mode!=0,mask,{},&result,work);
            if(status!=SfntStatus::ok && status!=SfntStatus::output_too_small)return status;
            if(!kind && !result.language_found)return SfntStatus::unsupported_sequence;
            if(!kind&&result.script_tag==sfnt_tag('l','a','t','n')){data.steps[0].generic=true;return SfntStatus::ok;}
            auto steps=take<SfntLookupStep>(kind?scratch:state,result.lookup_count);if(!filled(steps))return SfntStatus::resource_limit;
            status=sfnt_arabic_lookup_plan(layout,variations,mode!=0,mask,steps,&result,work);if(status!=SfntStatus::ok)return status;
            if(!kind)data.steps[mode]={steps.data,result.lookup_count};
            else{auto roots=take<u16>(state,result.lookup_count);if(!filled(roots))return SfntStatus::resource_limit;
                for(u32 i=0;i<result.lookup_count;++i)roots[i]=steps[i].lookup;
                data.shape.positioning_roots={roots.data,roots.count};}
        }
        if(!arena_rewind(&scratch,mark))return SfntStatus::invalid_input;
    }
    return native_positioning_prepare(*font,*base,data.shape.positioning_roots,state,scratch,&data.shape.positioning,work);
}
}
NativeFontResult native_arabic_plan_acquire(NativeArabicPlanStore* store,NativeRunPlanHandle run,Arena* scratch,NativeArabicPlanHandle* output,u32 work,usize limit) noexcept {
    if(output)reset_value(output);if(!store || !store->runs || !store->slots.data || !store->slots.count || !scratch || !scratch->base || !output || !native_run_plan_get(*store->runs,run))return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    if(store->slots.count>work)return failure(SfntStatus::resource_limit);
    u32 available=store->slots.count;
    for(u32 i=0;i<store->slots.count;++i){auto& value=store->slots[i];
        if(!value.references){if(available==store->slots.count && value.generation!=0xffffffffu)available=i;continue;}
        if(value.run.slot==run.slot && value.run.generation==run.generation){if(value.references==0xffffffffu)return {NativeFontError::capacity,SfntStatus::resource_limit,0};++value.references;*output={i+1,value.generation};return {NativeFontError::none,SfntStatus::ok,0};}
    }
    if(available==store->slots.count)return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    Win32SystemInfo info{};GetSystemInfo(info.native());const usize page=info.dwPageSize;if(!page || (page&(page-1)))return failure(SfntStatus::invalid_input);limit&=~(page-1);
    if(limit<page || limit<sizeof(PlanData))return failure(SfntStatus::resource_limit);
    Arena state{};struct Cleanup {Arena& state;~Cleanup() noexcept {arena_release(&state);}} cleanup{state};
    if(!arena_initialize(&state,limit,0,page))return {NativeFontError::capacity,SfntStatus::resource_limit,GetLastError()};
    auto* data=arena_allocate_zero<PlanData>(&state);if(!data)return failure(SfntStatus::resource_limit);
    arena_reset(scratch);const auto status=prepare(*store->runs,run,*data,state,*scratch,work);if(status!=SfntStatus::ok)return failure(status);
    DWORD old=0;if(!VirtualProtect(state.base,state.committed,PAGE_READONLY,&old))return {NativeFontError::io,SfntStatus::ok,GetLastError()};
    if(!native_run_plan_retain(store->runs,run))return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    auto& value=store->slots[available];value.run=run;value.state=state;reset_value(&state);value.references=1;++value.generation;*output={available+1,value.generation};return {NativeFontError::none,SfntStatus::ok,0};
}
NativeArabicPlanView native_arabic_plan_get(const NativeArabicPlanStore& store,NativeArabicPlanHandle handle,bool independent) noexcept {
    const auto* value=slot(store,handle);if(!value)return {};
    const auto& data=*reinterpret_cast<const PlanData*>(value->state.base);const u32 mode=independent?1u:0u;return {&data.shape,{data.steps[mode].data,data.steps[mode].count},data.cmap,data.steps[0].generic};
}
bool native_arabic_plan_release(NativeArabicPlanStore* store,NativeArabicPlanHandle handle) noexcept {
    if(!store)return false;auto* value=slot(*store,handle);if(!value)return false;if(--value->references)return true;
    const auto run=value->run;const u32 generation=value->generation;arena_release(&value->state);reset_value(value);value->generation=generation;return native_run_plan_release(store->runs,run);
}
}
