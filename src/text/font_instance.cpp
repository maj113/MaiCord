#include "maicord/text/font_instance.hpp"
#include "maicord/text/base.hpp"

#include <Windows.h>
namespace maicord {namespace {
NativeFontResult result(NativeFontError error,SfntStatus font=SfntStatus::ok,u32 native=0) noexcept {return {error,font,native};}
bool valid(const NativeFontInstanceStore& s) noexcept {return s.fonts&&s.slots.data&&s.slots.count;}
NativeFontInstanceSlot* slot(NativeFontInstanceStore& s,NativeFontInstanceHandle h) noexcept {
    if(!valid(s)||!h.slot||h.slot>s.slots.count||!h.generation)return nullptr;
    auto& p=s.slots[h.slot-1];return p.references&&p.generation==h.generation?&p:nullptr;
}
bool spend(u32& work,u32 n=1) noexcept {if(n>work)return false;work-=n;return true;}
template<class T> usize bytes(u32 n) noexcept {return (static_cast<usize>(n)*sizeof(T)+7)&~usize{7};}
template<class T> Slice<T> array(Arena& state,u32 count) noexcept {return {count?arena_allocate<T>(&state,count):nullptr,count};}
}
NativeFontResult native_font_instance_acquire(NativeFontInstanceStore* store,NativeFontHandle handle,
    ConstSlice<FontAxisSetting> settings,bool instructions,NativeFontPrepareScratch scratch,
    NativeFontInstanceHandle* output,u32 work,u32* used,usize state_limit) noexcept {
    struct Report {u32 initial;const u32& left;u32* used;~Report() noexcept {if(used)*used=initial-left;}} report{work,work,used};
    if(output)*output={};if(!store||!valid(*store)||!output)return result(NativeFontError::invalid_input);
    const auto* face=native_font_get(*store->fonts,handle);if(!face)return result(NativeFontError::invalid_input);
    const auto& geometry=face->geometry;const u32 axes=geometry.axes.axis_count;u32 child=0;
    if(instructions && geometry.kind!=FontGeometryKind::truetype)return result(NativeFontError::font,SfntStatus::unsupported);
    FontAxisMappings mappings{};auto status=font_axis_mappings_open(geometry.face,static_cast<u16>(axes),scratch.maps,&mappings,work,&child);work-=child;
    if(status==SfntStatus::ok){status=font_axis_coordinates(geometry.axes,mappings,settings,scratch.coordinates,{},FontAxisPrecision::fixed16,work,&child);work-=child;}
    if(status!=SfntStatus::ok)return result(NativeFontError::font,status);
    u32 free_slot=store->slots.count;
    for(u32 i=0;i<store->slots.count;++i){
        if(!spend(work))return result(NativeFontError::font,SfntStatus::resource_limit);auto& p=store->slots[i];
        if(!p.references){if(free_slot==store->slots.count&&p.generation!=0xffffffffu)free_slot=i;continue;}
        if(p.instance.face.slot!=handle.slot||p.instance.face.generation!=handle.generation||static_cast<bool>(p.instance.instructions)!=instructions)continue;
        bool equal=true;const auto* variation=p.instance.geometry.variation;
        if(axes){if(!spend(work,axes))return result(NativeFontError::font,SfntStatus::resource_limit);for(u32 a=0;a<axes;++a)if(variation->coordinates[a]!=scratch.coordinates[a])equal=false;}
        if(!equal)continue;if(p.references==0xffffffffu)return result(NativeFontError::capacity);
        ++p.references;*output={i+1,p.generation};return result(NativeFontError::none);
    }
    if(free_slot==store->slots.count)return result(NativeFontError::capacity);
    TtOutlineRequirements requirements{};usize required=0;
    if(axes)required=bytes<SfntGvarInstance>(1)+bytes<double>(axes)+bytes<double>(geometry.gvar.shared_count);
    if(instructions){status=tt_outline_requirements(geometry.face,&requirements);if(status!=SfntStatus::ok)return result(NativeFontError::font,status);
        required+=bytes<TtOutlineInstance>(1)+bytes<i32>(requirements.storage)+bytes<i32>(requirements.cvt)+bytes<TtFunction>(requirements.functions)+bytes<TtPoint>(requirements.twilight);
        if(static_cast<u64>(requirements.storage)+requirements.cvt+requirements.functions+requirements.twilight>work)return result(NativeFontError::font,SfntStatus::resource_limit);}
    usize page=0;if(required){SYSTEM_INFO info{};GetSystemInfo(&info);page=info.dwPageSize;
        if(!page||(page&(page-1)))return result(NativeFontError::invalid_input);required=(required+page-1)&~(page-1);}
    if(required>state_limit)return result(NativeFontError::font,SfntStatus::resource_limit);
    Arena state{};struct Cleanup {Arena& a;~Cleanup() noexcept {arena_release(&a);}} cleanup{state};
    if(required&&!arena_initialize(&state,required,required,4096))return result(NativeFontError::capacity,SfntStatus::ok,thread_last_error());
    NativeFontInstance instance{handle,{&geometry,nullptr},nullptr};
    if(axes){auto* variation=arena_allocate<SfntGvarInstance>(&state);auto coordinates=array<double>(state,axes),scalars=array<double>(state,geometry.gvar.shared_count);
        if(!variation||!coordinates.data||(scalars.count&&!scalars.data))return result(NativeFontError::capacity);
        if(!spend(work,axes))return result(NativeFontError::font,SfntStatus::resource_limit);copy_bytes(coordinates.data,scratch.coordinates.data,axes*sizeof(double));
        if(geometry.gvar.table.data){status=sfnt_gvar_instance_prepare(geometry.gvar,{coordinates.data,axes},scalars,variation,work,&child);work-=child;
            if(status!=SfntStatus::ok)return result(NativeFontError::font,status);}
        else *variation={{},{coordinates.data,axes},{}};
        instance.geometry.variation=variation;
    }
    if(instructions){auto* prepared=arena_allocate<TtOutlineInstance>(&state);const auto retained_end=arena_mark(&state);TtInterpreter buffers{};buffers.stack=scratch.stack;buffers.frames=scratch.frames;
        buffers.storage=array<i32>(state,requirements.storage);buffers.cvt=array<i32>(state,requirements.cvt);buffers.functions=array<TtFunction>(state,requirements.functions);buffers.twilight=array<TtPoint>(state,requirements.twilight);
        if(!prepared)return result(NativeFontError::capacity);TtExecution execution{};status=tt_outline_prepare(geometry,buffers,prepared,&execution,work,&child);work-=child;
        if(status!=SfntStatus::ok)return result(NativeFontError::font,status);
        if(prepared->graphics.instruction_control&1){
            // Disabled glyph programs cannot consume preparation arrays. Keep
            // the small profile header and any preceding variation state, and
            // decommit whole unused pages without allocating/copying again.
            prepared->storage={};prepared->cvt={};prepared->functions={};prepared->twilight={};prepared->stack_capacity=0;
            if(!arena_rewind(&state,retained_end))return result(NativeFontError::invalid_input);
            const usize committed=(state.offset+page-1)&~(page-1);
            if(committed<state.committed){if(!VirtualFree(state.base+committed,state.committed-committed,MEM_DECOMMIT))return result(NativeFontError::io,SfntStatus::ok,thread_last_error());state.committed=committed;}
        }
        instance.instructions=prepared;
    }
    if(state.base){DWORD old=0;if(!VirtualProtect(state.base,state.committed,PAGE_READONLY,&old))return result(NativeFontError::io,SfntStatus::ok,thread_last_error());}
    if(!native_font_retain(store->fonts,handle))return result(NativeFontError::capacity);
    auto& p=store->slots[free_slot];p.instance=instance;p.state=state;state={};p.references=1;++p.generation;
    *output={free_slot+1,p.generation};return result(NativeFontError::none);
}
bool native_font_instance_retain(NativeFontInstanceStore* s,NativeFontInstanceHandle h) noexcept {
    if(!s)return false;auto* p=slot(*s,h);if(!p||p->references==0xffffffffu)return false;++p->references;return true;
}
bool native_font_instance_release(NativeFontInstanceStore* s,NativeFontInstanceHandle h) noexcept {
    if(!s)return false;auto* p=slot(*s,h);if(!p)return false;if(--p->references)return true;
    const auto face=p->instance.face;const u32 generation=p->generation;arena_release(&p->state);*p={};p->generation=generation;return native_font_release(s->fonts,face);
}
const NativeFontInstance* native_font_instance_get(const NativeFontInstanceStore& s,NativeFontInstanceHandle h) noexcept {
    if(!valid(s)||!h.slot||h.slot>s.slots.count||!h.generation)return nullptr;
    const auto& p=s.slots[h.slot-1];return p.references&&p.generation==h.generation?&p.instance:nullptr;
}
} // namespace maicord
