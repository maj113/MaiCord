#include "run_profile.hpp"
#include "maicord/text/arabic_run_plan.hpp"

#include <Windows.h>

namespace maicord {namespace {
// The fallback flag occupies existing slice padding; retained plan size is unchanged.
struct StoredSteps {const SfntLookupStep* data;u32 count;bool generic=false;};
static_assert(sizeof(StoredSteps)==sizeof(ConstSlice<SfntLookupStep>));
struct PlanData {SfntShapePlan shape;StoredSteps steps[2];const SfntCmap* cmap;};
NativeFontResult failure(SfntStatus status) noexcept {return {NativeFontError::font,status,0};}
template<class T> Slice<T> take(Arena& arena,u32 count) noexcept {return {count?arena_allocate<T>(&arena,count):nullptr,count};}
template<class T> bool filled(Slice<T> values) noexcept {return !values.count || values.data;}
NativeArabicPlanSlot* slot(NativeArabicPlanStore& store,NativeArabicPlanHandle h) noexcept {
    if(!store.runs || !store.slots.data || !h.slot || h.slot>store.slots.count || !h.generation)return nullptr;
    auto& value=store.slots[h.slot-1];return value.references && value.generation==h.generation?&value:nullptr;
}
SfntStatus prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,PlanData& data,Arena& state,Arena& scratch,u32 work) noexcept {
    const auto* base=native_run_plan_get(store,run);if(!base || !store.instances || !store.instances->fonts)return SfntStatus::invalid_input;
    const auto* instance=native_font_instance_get(*store.instances,store.slots[run.slot-1].instance);if(!instance)return SfntStatus::invalid_input;
    const auto* font=native_font_get(*store.instances->fonts,instance->face);if(!font)return SfntStatus::invalid_input;
    data.cmap=&font->cmap;
    const auto coordinates=instance->geometry.variation?instance->geometry.variation->coordinates:ConstSlice<double>{};
    if(coordinates.count>work)return SfntStatus::resource_limit;
    auto fixed=take<i16>(scratch,coordinates.count);if(!filled(fixed))return SfntStatus::output_too_small;
    for(u32 i=0;i<coordinates.count;++i){const double value=coordinates[i]*16384;fixed[i]=static_cast<i16>(value+(value<0?-.5:.5));}
    data.shape=*base;data.shape.substitution_roots={};data.shape.positioning_roots={};
    for(u32 kind=0;kind<2;++kind){ConstSlice<u8> bytes{};auto status=sfnt_table(font->geometry.face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&bytes);
        if(status==SfntStatus::missing_table){if(!kind)return SfntStatus::unsupported_sequence;continue;}if(status!=SfntStatus::ok)return status;
        SfntLayout layout{};status=sfnt_layout_open(bytes,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout);if(status!=SfntStatus::ok)return status;
        SfntFeatureSubstitutions variations{};
        if(layout.variations){u32 record=0;status=sfnt_layout_variation_select(layout,{fixed.data,fixed.count},&variations,&record,work);if(status!=SfntStatus::ok)return status;}
        const auto mark=arena_mark(&scratch);auto mask=take<u8>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
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
    if(output)*output={};if(!store || !store->runs || !store->slots.data || !store->slots.count || !scratch || !scratch->base || !output || !native_run_plan_get(*store->runs,run))return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    if(store->slots.count>work)return failure(SfntStatus::resource_limit);
    u32 available=store->slots.count;
    for(u32 i=0;i<store->slots.count;++i){auto& value=store->slots[i];
        if(!value.references){if(available==store->slots.count && value.generation!=0xffffffffu)available=i;continue;}
        if(value.run.slot==run.slot && value.run.generation==run.generation){if(value.references==0xffffffffu)return {NativeFontError::capacity,SfntStatus::resource_limit,0};++value.references;*output={i+1,value.generation};return {NativeFontError::none,SfntStatus::ok,0};}
    }
    if(available==store->slots.count)return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    SYSTEM_INFO info{};GetSystemInfo(&info);const usize page=info.dwPageSize;if(!page || (page&(page-1)))return failure(SfntStatus::invalid_input);limit&=~(page-1);
    if(limit<page || limit<sizeof(PlanData))return failure(SfntStatus::resource_limit);
    Arena state{};struct Cleanup {Arena& state;~Cleanup() noexcept {arena_release(&state);}} cleanup{state};
    if(!arena_initialize(&state,limit,0,page))return {NativeFontError::capacity,SfntStatus::resource_limit,GetLastError()};
    auto* data=arena_allocate_zero<PlanData>(&state);if(!data)return failure(SfntStatus::resource_limit);
    arena_reset(scratch);const auto status=prepare(*store->runs,run,*data,state,*scratch,work);if(status!=SfntStatus::ok)return failure(status);
    DWORD old=0;if(!VirtualProtect(state.base,state.committed,PAGE_READONLY,&old))return {NativeFontError::io,SfntStatus::ok,GetLastError()};
    if(!native_run_plan_retain(store->runs,run))return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    auto& value=store->slots[available];value.run=run;value.state=state;state={};value.references=1;++value.generation;*output={available+1,value.generation};return {NativeFontError::none,SfntStatus::ok,0};
}
NativeArabicPlanView native_arabic_plan_get(const NativeArabicPlanStore& store,NativeArabicPlanHandle handle,bool independent) noexcept {
    const auto* value=slot(const_cast<NativeArabicPlanStore&>(store),handle);if(!value)return {};
    const auto& data=*reinterpret_cast<const PlanData*>(value->state.base);const u32 mode=independent?1u:0u;return {&data.shape,{data.steps[mode].data,data.steps[mode].count},data.cmap,data.steps[0].generic};
}
bool native_arabic_plan_release(NativeArabicPlanStore* store,NativeArabicPlanHandle handle) noexcept {
    if(!store)return false;auto* value=slot(*store,handle);if(!value)return false;if(--value->references)return true;
    const auto run=value->run;const u32 generation=value->generation;arena_release(&value->state);*value={};value->generation=generation;return native_run_plan_release(store->runs,run);
}
}
