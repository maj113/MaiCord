#pragma once
#include "maicord/ui/text.hpp"
namespace maicord {
struct AsciiBreakReference final : IDWriteTextAnalysisSource,IDWriteTextAnalysisSink {
    wchar_t units[256];DWRITE_LINE_BREAKPOINT points[256];u32 count;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(bytes_equal(&id,&__uuidof(IUnknown),sizeof(GUID))||bytes_equal(&id,&__uuidof(IDWriteTextAnalysisSource),sizeof(GUID)))*output=static_cast<IDWriteTextAnalysisSource*>(this);
        else if(bytes_equal(&id,&__uuidof(IDWriteTextAnalysisSink),sizeof(GUID)))*output=static_cast<IDWriteTextAnalysisSink*>(this);else return E_NOINTERFACE;return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 at,const wchar_t** value,UINT32* length) override {*value=at<count?units+at:nullptr;*length=at<count?count-at:0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 at,const wchar_t** value,UINT32* length) override {if(at>count)return E_INVALIDARG;*value=at?units:nullptr;*length=at;return S_OK;}
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;}
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 at,UINT32* length,const wchar_t** value) override {if(at>count)return E_INVALIDARG;*length=count-at;*value=L"en-US";return S_OK;}
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 at,UINT32* length,IDWriteNumberSubstitution** value) override {if(at>count)return E_INVALIDARG;*length=count-at;*value=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32,UINT32,const DWRITE_SCRIPT_ANALYSIS*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32 at,UINT32 length,const DWRITE_LINE_BREAKPOINT* values) override {if(at>count||length>count-at)return E_INVALIDARG;for(u32 i=0;i<length;++i)points[at+i]=values[i];return S_OK;}
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32,UINT32,IDWriteNumberSubstitution*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32,UINT32,UINT8,UINT8) override {return E_NOTIMPL;}
    bool analyze(IDWriteTextAnalyzer* analyzer,Utf8View text) noexcept {if(text.bytes>256)return false;count=text.bytes;for(u32 i=0;i<count;++i){units[i]=static_cast<u8>(text.data[i]);points[i]={};}return SUCCEEDED(analyzer->AnalyzeLineBreakpoints(this,0,count,this));}
    bool after(u32 at) const noexcept {if(!at)return false;if(at==count)return true;const auto condition=maximum<u32>(points[at-1].breakConditionAfter,points[at].breakConditionBefore);return condition!=DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;}
};
}
