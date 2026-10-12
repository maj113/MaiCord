#pragma once
#include "dwrite_text_system.hpp"
#include "maicord/text/font_run.hpp"
namespace maicord {
// QA only: reconstruct a Windows oracle face from an already retained native
// record. Production rendering never calls this adapter. Output owns one ref.
inline bool native_run_reference(IDWriteFactory* base,const ShapedText& text,const TextRun& run,IDWriteFontFace** output) noexcept {
    if(!output)return false;*output=nullptr;
    if(!run.native_font){auto* face=reference_run_face(run);if(!face)return false;face->AddRef();*output=face;return true;}
    if(!base||!text.native_fonts())return false;
    const auto* record=native_font_run_get(*native_font_session_runs(text.native_fonts()),run.native_face);if(!record)return false;
    const auto* store=native_font_session_fonts(text.native_fonts());const auto* font=native_font_get(*store,record->source.face);if(!font)return false;
    const auto file_index=store->faces[record->source.face.slot-1].file_slot;wchar_t path[32768];
    const u32 length=GetFinalPathNameByHandleW(store->files[file_index].file,path,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!length||length>=32768)return false;
    IDWriteFactory6* factory=nullptr;IDWriteFontFile* file=nullptr;IDWriteFontResource* resource=nullptr;IDWriteFontFace5* face=nullptr;
    struct Release {IDWriteFactory6*& a;IDWriteFontFile*& b;IDWriteFontResource*& c;IDWriteFontFace5*& d;~Release() noexcept {com_release(d);com_release(c);com_release(b);com_release(a);}} release{factory,file,resource,face};
    auto hr=base->QueryInterface(__uuidof(IDWriteFactory6),reinterpret_cast<void**>(&factory));
    // Local DirectWrite reference keys distinguish extended and ordinary DOS
    // spellings even for the same file. Match its ordinary system-file spelling.
    const wchar_t* local=length>6&&path[0]==L'\\'&&path[1]==L'\\'&&path[2]==L'?'&&path[3]==L'\\'&&path[5]==L':'?path+4:path;
    if(SUCCEEDED(hr))hr=base->CreateFontFileReference(local,nullptr,&file);
    if(SUCCEEDED(hr))hr=factory->CreateFontResource(file,font->geometry.face.face_index,&resource);
    DWRITE_FONT_AXIS_VALUE axes[2];for(u32 i=0;i<record->source.count;++i){const auto setting=record->source.settings[i];const u32 tag=setting.tag;
        axes[i]={static_cast<DWRITE_FONT_AXIS_TAG>(((tag&255)<<24)|((tag&0xff00)<<8)|((tag>>8)&0xff00)|(tag>>24)),static_cast<float>(setting.value)};}
    if(SUCCEEDED(hr))hr=resource->CreateFontFace(static_cast<DWRITE_FONT_SIMULATIONS>(record->simulations),axes,record->source.count,&face);
    if(FAILED(hr)||!face)return false;*output=face;face=nullptr;return true;
}
}
