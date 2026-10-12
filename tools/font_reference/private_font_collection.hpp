#pragma once
// QA-only private collections. The ordinary app-compatible path exposes face
// zero; controlled family probes may explicitly expose all collection faces.
#include <dwrite_3.h>
namespace maicord::font_reference {
inline HRESULT private_collection(IDWriteFactory3* factory,const wchar_t* path,bool all_faces,IDWriteFontCollection** output) noexcept {
    *output=nullptr;IDWriteFontFile* file=nullptr;IDWriteFontSetBuilder* builder=nullptr;IDWriteFontSet* set=nullptr;
    struct Cleanup {IDWriteFontFile*& file;IDWriteFontSetBuilder*& builder;IDWriteFontSet*& set;
        ~Cleanup() noexcept{if(set)set->Release();if(builder)builder->Release();if(file)file->Release();}} cleanup{file,builder,set};
    UINT32 count=1;HRESULT hr=S_OK;
    if(all_faces){hr=factory->CreateFontFileReference(path,nullptr,&file);if(FAILED(hr))return hr;
        BOOL supported=FALSE;DWRITE_FONT_FILE_TYPE file_type{};DWRITE_FONT_FACE_TYPE face_type{};
        hr=file->Analyze(&supported,&file_type,&face_type,&count);if(FAILED(hr))return hr;if(!supported||!count||count>1024)return E_INVALIDARG;}
    hr=factory->CreateFontSetBuilder(&builder);if(FAILED(hr))return hr;
    for(UINT32 i=0;i<count;++i){IDWriteFontFaceReference* reference=nullptr;
        hr=factory->CreateFontFaceReference(path,nullptr,i,DWRITE_FONT_SIMULATIONS_NONE,&reference);if(FAILED(hr))return hr;
        hr=builder->AddFontFaceReference(reference);reference->Release();if(FAILED(hr))return hr;}
    hr=builder->CreateFontSet(&set);if(FAILED(hr))return hr;IDWriteFontCollection1* collection=nullptr;
    hr=factory->CreateFontCollectionFromFontSet(set,&collection);if(SUCCEEDED(hr))*output=collection;return hr;
}
inline bool all_faces_option(const wchar_t* option) noexcept {
    constexpr wchar_t expected[]=L"--all-faces";UINT32 i=0;while(expected[i]&&option[i]==expected[i])++i;return !expected[i]&&!option[i];
}
}
