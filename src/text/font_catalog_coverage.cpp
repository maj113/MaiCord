#include "maicord/text/font_catalog_coverage.hpp"
namespace maicord {namespace {
NativeFontResult invalid() noexcept{return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};}
bool valid(const NativeFontCoverageCache* c) noexcept{return c&&c->session&&c->slots.data&&c->slots.count&&c->slots.count<=64;}
u32 tick(NativeFontCoverageCache& c) noexcept {
    if(c.clock==0xffffffffu){u32 ranks[64]{},live=0;
        for(u32 i=0;i<c.slots.count;++i)if(c.slots[i].handle.slot){++live;u32 rank=1;
            for(u32 j=0;j<c.slots.count;++j)if(c.slots[j].handle.slot&&c.slots[j].stamp<c.slots[i].stamp)++rank;ranks[i]=rank;}
        for(u32 i=0;i<c.slots.count;++i)c.slots[i].stamp=ranks[i];c.clock=live;
    }return ++c.clock;
}
NativeFontResult lookup(NativeFontCoverageCache* c,const NativeFontCatalog& catalog,u32 candidate,NativeFontHandle* output,bool refresh=false,NativeFontHandle ready={}) noexcept {
    *output={};if(!valid(c))return invalid();NativeFontCatalogEntry entry{};auto result=native_font_catalog_entry(catalog,candidate,&entry);if(!result.ok())return result;
    const u32 face=catalog.candidates[candidate].face;auto* fonts=native_font_session_fonts(c->session);u32 victim=0xffffffffu;
    for(u32 i=0;i<c->slots.count;++i){auto& slot=c->slots[i];
        if(slot.handle.slot&&slot.catalog==&catalog&&slot.face==face){
            if(refresh){victim=i;break;}
            if(!native_font_get(*fonts,slot.handle))return invalid();slot.stamp=tick(*c);*output=slot.handle;return {};}
        if(victim==0xffffffffu||(!slot.handle.slot&&c->slots[victim].handle.slot)||(slot.handle.slot&&c->slots[victim].handle.slot&&slot.stamp<c->slots[victim].stamp))victim=i;
    }
    auto& slot=c->slots[victim];if(slot.handle.slot){if(!native_font_release(fonts,slot.handle))return invalid();
        if(slot.catalog!=&catalog||slot.face!=face)++c->evictions;slot={};}
    NativeFontHandle handle=ready;
    if(ready.slot){if(!native_font_retain(fonts,ready))return invalid();}
    else {result=native_font_session_face(c->session,reinterpret_cast<const wchar_t*>(entry.path.data),entry.face_index,&handle);if(!result.ok())return result;}
    result=native_font_catalog_validate(entry,*fonts,handle);if(!result.ok()){native_font_release(fonts,handle);return result;}
    slot={&catalog,handle,face,tick(*c)};++c->acquisitions;*output=handle;return {};
}
}
bool native_font_coverage_initialize(NativeFontSession* session,Slice<NativeFontCoverageSlot> slots,NativeFontCoverageCache* output) noexcept {
    if(!session||!output||output->session||output->slots.data||output->slots.count||!slots.data||!slots.count||slots.count>64)return false;
    for(u32 i=0;i<slots.count;++i)if(slots[i].catalog||slots[i].handle.slot||slots[i].handle.generation||slots[i].face||slots[i].stamp)return false;
    if(!native_font_session_retain(session))return false;*output={session,slots,0,0,0};return true;
}
bool native_font_coverage_reset(NativeFontCoverageCache* c) noexcept {
    if(!valid(c))return false;auto* fonts=native_font_session_fonts(c->session);bool okay=true;
    for(u32 i=0;i<c->slots.count;++i){auto& slot=c->slots[i];if(slot.handle.slot&&!native_font_release(fonts,slot.handle)){okay=false;continue;}slot={};}
    if(okay)c->clock=0;return okay;
}
bool native_font_coverage_release(NativeFontCoverageCache* c) noexcept {
    if(!c)return false;if(!c->session){*c={};return true;}
    if(!native_font_coverage_reset(c)||!native_font_session_release(&c->session))return false;*c={};return true;
}
NativeFontResult native_font_coverage_probe(NativeFontCoverageCache* c,const NativeFontCatalog& catalog,u32 candidate,u32 scalar,bool* output,const NativeFontFace** borrowed) noexcept {
    if(borrowed)*borrowed=nullptr;
    if(output)*output=false;if(!output||scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff))return invalid();
    NativeFontHandle handle{};auto result=lookup(c,catalog,candidate,&handle);if(!result.ok())return result;
    const auto* font=native_font_get(*native_font_session_fonts(c->session),handle);if(!font)return invalid();u16 glyph=0;
    const auto status=sfnt_cmap_glyph(font->cmap,scalar,0,&glyph);if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};*output=glyph!=0;if(borrowed)*borrowed=font;return {};
}
NativeFontResult native_font_coverage_acquire(NativeFontCoverageCache* c,const NativeFontCatalog& catalog,u32 candidate,NativeFontHandle* output,bool refresh) noexcept {
    if(output)*output={};if(!output)return invalid();NativeFontHandle handle{};auto result=lookup(c,catalog,candidate,&handle,refresh);if(!result.ok())return result;
    if(!native_font_retain(native_font_session_fonts(c->session),handle))return {NativeFontError::capacity,SfntStatus::resource_limit,0};*output=handle;return {};
}
NativeFontResult native_font_coverage_remember(NativeFontCoverageCache* c,const NativeFontCatalog& catalog,u32 candidate,NativeFontHandle ready) noexcept {
    if(!ready.slot)return invalid();NativeFontHandle borrowed{};return lookup(c,catalog,candidate,&borrowed,true,ready);
}
}
