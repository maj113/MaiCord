#include "maicord/text/font_run.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {namespace {
NativeFontRunSlot* slot(const NativeFontRunStore* store,NativeFontRunHandle h) noexcept {
    if(!store||!h.slot||h.slot>store->slots.count)return nullptr;auto& s=store->slots[h.slot-1];return s.references&&s.generation==h.generation?&s:nullptr;
}
NativeFontResult invalid() noexcept {return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};}
}
NativeFontResult native_font_run_acquire(NativeFontRunStore* store,NativeFontInstanceHandle instance,const NativeFontSelection& source,u32 simulations,NativeFontRunHandle* output) noexcept {
    if(output)reset_value(output);if(!store||!store->session||!output||source.count>2||simulations>3)return invalid();
    const auto* raw=native_font_instance_get(*native_font_session_instances(store->session),instance);
    if(!raw||raw->face.slot!=source.face.slot||raw->face.generation!=source.face.generation||raw->geometry.font->axes.axis_count!=source.count)return invalid();
    for(u32 i=0;i<source.count;++i){FontAxis axis{};if(font_axis(raw->geometry.font->axes,i,&axis)!=SfntStatus::ok||axis.tag!=source.settings[i].tag)return invalid();}
    u32 vacant=store->slots.count;
    for(u32 i=0;i<store->slots.count;++i){auto& s=store->slots[i];
        if(!s.references){if(vacant==store->slots.count&&s.generation!=0xffffffffu)vacant=i;continue;}
        if(s.run.instance.slot==instance.slot&&s.run.instance.generation==instance.generation&&s.run.simulations==simulations){
            if(s.references==0xffffffffu)return {NativeFontError::capacity,SfntStatus::resource_limit,0};++s.references;*output={i+1,s.generation};return {NativeFontError::none,SfntStatus::ok,0};}}
    if(vacant==store->slots.count)return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    if(!native_font_instance_retain(native_font_session_instances(store->session),instance))return invalid();
    auto& saved=store->slots[vacant];saved.run={source,instance,simulations};saved.references=1;++saved.generation;*output={vacant+1,saved.generation};return {NativeFontError::none,SfntStatus::ok,0};
}
const NativeFontRun* native_font_run_get(const NativeFontRunStore& store,NativeFontRunHandle h) noexcept {
    const auto* s=slot(&store,h);return s?&s->run:nullptr;
}
bool native_font_run_retain(NativeFontRunStore* store,NativeFontRunHandle h) noexcept {auto* s=slot(store,h);if(!s||s->references==0xffffffffu)return false;++s->references;return true;}
bool native_font_run_release(NativeFontRunStore* store,NativeFontRunHandle h) noexcept {
    auto* s=slot(store,h);if(!s)return false;if(s->references==1){if(!native_font_instance_release(native_font_session_instances(store->session),s->run.instance))return false;reset_value(&s->run);}--s->references;return true;
}
NativeFontResult native_font_run_prepare(NativeFontRunStore* store,NativeFontRunHandle h,Arena* scratch,NativeFontInstanceHandle* output,bool* oblique,bool* bold,bool instructions) noexcept {
    if(output)reset_value(output);if(oblique)*oblique=false;if(bold)*bold=false;auto* s=slot(store,h);
    if(!s||!scratch||!scratch->base||!output)return invalid();arena_reset(scratch);
    return native_font_prepare(store->session,s->run.source.face,{s->run.source.settings,s->run.source.count},s->run.simulations,instructions,scratch,output,oblique,bold);
}
}
