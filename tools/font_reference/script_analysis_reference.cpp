// QA only: observe paragraph script analysis independently of font selection.
#include "maicord/core/base.hpp"
#include "maicord/core/memory.hpp"
#include <dwrite_1.h>
namespace maicord {namespace {
template<u32 Capacity> struct Analysis final : IDWriteTextAnalysisSource,IDWriteTextAnalysisSink {
    wchar_t text[Capacity]{};DWRITE_SCRIPT_ANALYSIS scripts[Capacity]{};u32 length=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID))||bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSource),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSource*>(this);
        else if(bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSink),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSink*>(this);
        return *out?S_OK:E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;} ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 at,WCHAR const** out,UINT32* count) override {
        if(!out||!count)return E_POINTER;*out=at<length?text+at:nullptr;*count=at<length?length-at:0;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 at,WCHAR const** out,UINT32* count) override {
        if(!out||!count)return E_POINTER;if(at>length)return E_INVALIDARG;*out=at?text:nullptr;*count=at;return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;}
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 at,UINT32* count,WCHAR const** name) override {
        if(!count||!name)return E_POINTER;*count=at<length?length-at:0;*name=L"en-US";return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 at,UINT32* count,IDWriteNumberSubstitution** value) override {
        if(!count||!value)return E_POINTER;*count=at<length?length-at:0;*value=nullptr;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32 at,UINT32 count,DWRITE_SCRIPT_ANALYSIS const* script) override {
        if(!script||at>length||count>length-at)return E_INVALIDARG;
        for(u32 i=at;i<at+count;++i)scripts[i]=*script;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32,UINT32,DWRITE_LINE_BREAKPOINT const*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32,UINT32,UINT8,UINT8) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32,UINT32,IDWriteNumberSubstitution*) override {return E_NOTIMPL;}
};
constexpr u32 contexts[]{0,0x61,0x628,0x5d0,0x4e2d,0x915,0xe01,0x3042};
constexpr u32 script_capacity=2048;
DWRITE_SCRIPT_PROPERTIES properties[script_capacity]{};bool seen[script_capacity]{};u32 script_end=0;
u8 buffer[65532];u32 buffered=0;u64 observations=0;
bool write(const void* data,u32 bytes) noexcept {
    DWORD done=0;return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),data,bytes,&done,nullptr)&&done==bytes;
}
bool flush() noexcept {const bool okay=!buffered||write(buffer,buffered);buffered=0;return okay;}
bool save(const void* data,u32 bytes) noexcept {
    if(bytes>sizeof(buffer))return false;if(bytes>sizeof(buffer)-buffered&&!flush())return false;
    copy_bytes(buffer+buffered,data,bytes);buffered+=bytes;return true;
}
bool packed(IDWriteTextAnalyzer1* analyzer,DWRITE_SCRIPT_ANALYSIS script,u16* out) noexcept {
    if(script.script>=script_capacity||(script.shapes!=DWRITE_SCRIPT_SHAPES_DEFAULT&&script.shapes!=DWRITE_SCRIPT_SHAPES_NO_VISUAL))return false;
    if(!seen[script.script]){if(FAILED(analyzer->GetScriptProperties(script,properties+script.script)))return false;seen[script.script]=true;script_end=maximum(script_end,static_cast<u32>(script.script)+1);}
    *out=static_cast<u16>(script.script|(script.shapes==DWRITE_SCRIPT_SHAPES_NO_VISUAL?0x8000:0));return true;
}
bool observe(IDWriteTextAnalyzer1* analyzer) noexcept {
    constexpr u32 count=sizeof(contexts)/sizeof(contexts[0]);
    const u32 header[]{0x3141534d,1,count,0x110000,6};if(!save(header,sizeof(header))||!save(contexts,sizeof(contexts)))return false;
    Analysis<8> input;
    for(u32 cp=0;cp<0x110000;++cp)for(u32 context:contexts){u16 row[3]{0xffff,0xffff,0xffff};
        if(cp<0xd800||cp>0xdfff){input.length=0;if(context)input.text[input.length++]=static_cast<wchar_t>(context);const u32 first=input.length;
            if(cp>0xffff){input.text[input.length++]=static_cast<wchar_t>(0xd800+((cp-0x10000)>>10));input.text[input.length++]=static_cast<wchar_t>(0xdc00+((cp-0x10000)&1023));}
            else input.text[input.length++]=static_cast<wchar_t>(cp);
            const u32 end=input.length;if(context)input.text[input.length++]=static_cast<wchar_t>(context);
            for(u32 i=0;i<input.length;++i)input.scripts[i]={0x7fff,DWRITE_SCRIPT_SHAPES_DEFAULT};
            if(FAILED(analyzer->AnalyzeScript(&input,0,input.length,&input))||!packed(analyzer,input.scripts[first],row+1))return false;
            if(end-first==2&&(input.scripts[first].script!=input.scripts[first+1].script||input.scripts[first].shapes!=input.scripts[first+1].shapes))return false;
            if(context&&(!packed(analyzer,input.scripts[0],row)||!packed(analyzer,input.scripts[end],row+2)))return false;
            ++observations;}
        if(!save(row,sizeof(row)))return false;
    }
    static_assert(sizeof(DWRITE_SCRIPT_PROPERTIES)==20);
    return save(&script_end,4)&&save(properties,script_end*sizeof(DWRITE_SCRIPT_PROPERTIES))&&flush();
}
int run() noexcept {
    IDWriteFactory* factory=nullptr;IDWriteTextAnalyzer* base=nullptr;IDWriteTextAnalyzer1* analyzer=nullptr;
    auto hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateTextAnalyzer(&base);if(SUCCEEDED(hr))hr=base->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&analyzer));
    const bool okay=SUCCEEDED(hr)&&observe(analyzer);
    if(analyzer)analyzer->Release();if(base)base->Release();if(factory)factory->Release();
    const char* message=okay?"PASS exhaustive script analysis\n":"FAIL script analysis\n";u32 length=0;while(message[length])++length;DWORD written=0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,length,&written,nullptr);return okay?0:1;
}
}}
extern "C" int maicord_script_analysis_reference_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
