#include "maicord/text/font_catalog_prepare.hpp"
namespace maicord {
NativeFontResult native_font_catalog_prepare(NativeFontSession* session,const NativeFontCatalog& catalog,u32 candidate,u32 simulations,bool instructions,Arena* scratch,NativeFontInstanceHandle* output,bool* oblique,bool* bold) noexcept {
    if(output)*output={};if(oblique)*oblique=false;if(bold)*bold=false;
    if(!session||!output||!scratch||!scratch->base)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    // Reject unpermitted simulations before opening a font or growing a pool.
    if(simulations>3||((simulations&2)&&!oblique)||((simulations&1)&&!bold))return {NativeFontError::font,SfntStatus::unsupported,0};
    NativeFontCatalogEntry entry{};auto result=native_font_catalog_entry(catalog,candidate,&entry);if(!result.ok())return result;
    NativeFontHandle face{};result=native_font_session_face(session,reinterpret_cast<const wchar_t*>(entry.path.data),entry.face_index,&face);if(!result.ok())return result;
    auto* fonts=native_font_session_fonts(session);struct Release {NativeFontStore* fonts;NativeFontHandle face;~Release() noexcept {native_font_release(fonts,face);}} release{fonts,face};
    result=native_font_catalog_validate(entry,*fonts,face);if(!result.ok())return result;
    return native_font_prepare(session,face,entry.settings,simulations,instructions,scratch,output,oblique,bold);
}
}
