#include "maicord/text/font_catalog_coverage.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {namespace {
// The slot holding this catalog, else the least recently used slot; an empty
// slot has stamp zero, below any used stamp.
NativeFontCoverageSlot& slot_for(NativeFontCoverageCache& c,const NativeFontCatalog& catalog) noexcept {
    u32 victim=0;
    for(u32 i=0;i<c.slots.count;++i){if(c.slots[i].catalog==&catalog)return c.slots[i];if(c.slots[i].stamp<c.slots[victim].stamp)victim=i;}
    return c.slots[victim];
}
// Drops the slot's reference before it is reused; the cache owns every reference it holds.
void evict(NativeFontCoverageCache& c,NativeFontCoverageSlot& slot,const NativeFontCatalog& catalog) noexcept {
    if(!slot.catalog)return;
    native_font_release(native_font_session_fonts(c.session),slot.handle);if(slot.catalog!=&catalog)++c.evictions;slot={};
}
NativeFontResult lookup(NativeFontCoverageCache* c,const NativeFontCatalog& catalog,NativeFontHandle* output,bool refresh) noexcept {
    auto& slot=slot_for(*c,catalog);
    if(slot.catalog==&catalog&&!refresh){slot.stamp=++c->clock;*output=slot.handle;return {};}
    evict(*c,slot,catalog);auto* fonts=native_font_session_fonts(c->session);NativeFontHandle handle{};
    auto result=native_font_session_face(c->session,catalog.path,catalog.face_index,&handle);if(!result.ok())return result;
    result=native_font_catalog_validate(catalog,*fonts,handle);if(!result.ok()){native_font_release(fonts,handle);return result;}
    slot={&catalog,handle,++c->clock};++c->acquisitions;*output=handle;return {};
}
}
bool native_font_coverage_initialize(NativeFontSession* session,Slice<NativeFontCoverageSlot> slots,NativeFontCoverageCache* output) noexcept {
    if(!native_font_session_retain(session))return false;*output={session,slots,0,0,0};return true;
}
bool native_font_coverage_reset(NativeFontCoverageCache* c) noexcept {
    auto* fonts=native_font_session_fonts(c->session);bool okay=true;
    for(u32 i=0;i<c->slots.count;++i){auto& slot=c->slots[i];if(slot.catalog&&!native_font_release(fonts,slot.handle)){okay=false;continue;}slot={};}
    if(okay)c->clock=0;return okay;
}
bool native_font_coverage_release(NativeFontCoverageCache* c) noexcept {
    if(!c->session){reset_value(c);return true;}
    if(!native_font_coverage_reset(c)||!native_font_session_release(&c->session))return false;reset_value(c);return true;
}
NativeFontResult native_font_coverage_probe(NativeFontCoverageCache* c,const NativeFontCatalog& catalog,u32 scalar,bool* output,const NativeFontFace** borrowed) noexcept {
    if(borrowed)*borrowed=nullptr;
    *output=false;NativeFontHandle handle{};auto result=lookup(c,catalog,&handle,false);if(!result.ok())return result;
    const auto* font=native_font_get(*native_font_session_fonts(c->session),handle);u16 glyph=0;
    const auto status=sfnt_cmap_glyph(font->cmap,scalar,0,&glyph);if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};*output=glyph!=0;if(borrowed)*borrowed=font;return {};
}
NativeFontResult native_font_coverage_acquire(NativeFontCoverageCache* c,const NativeFontCatalog& catalog,NativeFontHandle* output,bool refresh) noexcept {
    reset_value(output);NativeFontHandle handle{};auto result=lookup(c,catalog,&handle,refresh);if(!result.ok())return result;
    if(!native_font_retain(native_font_session_fonts(c->session),handle))return {NativeFontError::capacity,SfntStatus::resource_limit,0};*output=handle;return {};
}
NativeFontResult native_font_coverage_remember(NativeFontCoverageCache* c,const NativeFontCatalog& catalog,NativeFontHandle ready) noexcept {
    auto& slot=slot_for(*c,catalog);evict(*c,slot,catalog);
    if(!native_font_retain(native_font_session_fonts(c->session),ready))return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    slot={&catalog,ready,++c->clock};++c->acquisitions;return {};
}
}
