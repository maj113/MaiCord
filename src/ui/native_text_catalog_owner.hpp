#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/gdi.hpp"
namespace maicord {
// Context-local metadata for one physical face; never serialized as a full file.
struct NativeSystemFontFile {NativeSystemFontFile* next;NativeFontCatalog catalog;u32 first_candidate;};
// Maps a GDI font description to its file path and face index.
struct SystemFontLocator {
    struct Realization {DWORD bytes,flags,cache,instance,files;WORD face,simulations;};
    struct File {FILETIME write;LARGE_INTEGER size;wchar_t path[1024];};
    static_assert(sizeof(Realization)==24&&sizeof(File)==2064);
    using Realize=BOOL(WINAPI*)(HDC,Realization*);
    using Locate=BOOL(WINAPI*)(DWORD,DWORD,void*,SIZE_T,SIZE_T*);
    HMODULE module=nullptr;HDC dc=nullptr;Realize realize=nullptr;Locate locate=nullptr;
    void release() noexcept {if(dc)DeleteDC(dc);if(module)FreeLibrary(module);dc=nullptr;module=nullptr;}
    bool initialize() noexcept {
        module=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!module)return false;
        realize=reinterpret_cast<Realize>(GetProcAddress(module,"GetFontRealizationInfo"));
        locate=reinterpret_cast<Locate>(GetProcAddress(module,"GetFontFileInfo"));
        return realize&&locate&&(dc=CreateCompatibleDC(nullptr));
    }
    bool find(const LOGFONTW& description,File& file,u32* index,bool exact_name) noexcept {
        file={};const HFONT font=CreateFontIndirectW(&description);if(!font)return false;const auto previous=SelectObject(dc,font);
        struct Restore {HDC dc;HGDIOBJ previous;HFONT font;~Restore() noexcept {if(previous&&previous!=HGDI_ERROR)SelectObject(dc,previous);DeleteObject(font);}} restore{dc,previous,font};
        if(!previous||previous==HGDI_ERROR)return false;
        if(exact_name){wchar_t realized[LF_FACESIZE]{};
            if(!GetTextFaceW(dc,LF_FACESIZE,realized)||CompareStringOrdinal(description.lfFaceName,-1,realized,-1,TRUE)!=CSTR_EQUAL)return false;}
        Realization info{sizeof(Realization)};SIZE_T needed=0;
        if(!realize(dc,&info)||info.files!=1)return false;*index=info.face;
        // Named instances in a noncollection variable font share physical face zero.
        if(info.face&&GetFontData(dc,0x66637474,0,nullptr,0)==GDI_ERROR&&GetFontData(dc,0x72617666,0,nullptr,0)!=GDI_ERROR)*index=0;
        return locate(info.instance,0,&file,sizeof(file),&needed)&&
            needed<=sizeof(file)&&file.path[0]&&!file.path[1023]&&file.size.QuadPart>0&&file.size.QuadPart<=0xffffffffu;
    }
};
}
