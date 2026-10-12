// QA only. Observe real TextLayout script selection across font/size cuts.
#define maicord_gsub_execution_reference_entry maicord_unused_gsub_reference_entry
#include "gsub_execution_reference.cpp"
#undef maicord_gsub_execution_reference_entry
#include "maicord/core/utf.hpp"
#include "maicord/ui/emoji_presentation.hpp"
namespace maicord {namespace {
struct CutCapture final : IDWriteTextRenderer {
    bool comma=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID))&&!bytes_equal(&iid,&__uuidof(IDWriteTextRenderer),sizeof(GUID))&&!bytes_equal(&iid,&__uuidof(IDWritePixelSnapping),sizeof(GUID)))return E_NOINTERFACE;
        *out=static_cast<IDWriteTextRenderer*>(this);return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;} ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* value) override {*value=TRUE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* value) override {*value={1,0,0,1,0,0};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value) override {*value=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION* desc,IUnknown*) override {
        if(!run||!desc)return E_INVALIDARG;
        IDWriteFontFace3* face=nullptr;IDWriteLocalizedStrings* names=nullptr;wchar_t family[256]{};
        auto hr=run->fontFace->QueryInterface(__uuidof(IDWriteFontFace3),reinterpret_cast<void**>(&face));
        if(SUCCEEDED(hr))hr=face->GetFamilyNames(&names);if(SUCCEEDED(hr))hr=names->GetString(0,family,256);
        const bool color=SUCCEEDED(hr)&&face->IsColorFont();com_release(names);com_release(face);if(FAILED(hr))return hr;
        const u32 font=bytes_equal(family,L"MaiCord Cut Emoji",sizeof(L"MaiCord Cut Emoji"))?1u:bytes_equal(family,L"MaiCord Cut Text",sizeof(L"MaiCord Cut Text"))?0u:2u;
        if(comma)print(utf8_literal(","));comma=true;
        field(utf8_literal("{\"font\":"),font);field(utf8_literal(",\"color_font\":"),color?1u:0u);field(utf8_literal(",\"first\":"),desc->textPosition);field(utf8_literal(",\"length\":"),desc->stringLength);field(utf8_literal(",\"bidi\":"),run->bidiLevel);
        print(utf8_literal(",\"origin_em_bits\":["));const float values[]{x,y,run->fontEmSize};for(u32 i=0;i<3;++i){u32 bits=0;copy_bytes(&bits,values+i,4);if(i)print(utf8_literal(","));number(bits);}print(utf8_literal("],\"glyphs\":["));
        for(u32 i=0;i<run->glyphCount;++i){if(i)print(utf8_literal(","));number(run->glyphIndices[i]);}
        print(utf8_literal("],\"clusters\":["));for(u32 i=0;i<desc->stringLength;++i){if(i)print(utf8_literal(","));number(desc->clusterMap[i]);}
        print(utf8_literal("]}"));return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override {return E_NOTIMPL;}
};
int cut_main() noexcept {
    const wchar_t* command=GetCommandLineW();wchar_t executable[32768],paths[2][32768],encoded[4096],mode[32],direction[16];
    if(!argument(&command,executable,32768)||!argument(&command,paths[0],32768)||!argument(&command,paths[1],32768)||!argument(&command,encoded,4096)||!argument(&command,mode,32)||!argument(&command,direction,16))return 2;
    Analysis analysis;if(!parse_text(encoded,&analysis))return 2;
    if(!bytes_equal(direction,L"0",sizeof(L"0"))&&!bytes_equal(direction,L"1",sizeof(L"1")))return 2;analysis.rtl=direction[0]=='1';
    const bool whole=bytes_equal(mode,L"whole",sizeof(L"whole")),sizes=bytes_equal(mode,L"sizes",sizeof(L"sizes"));
    if(!whole&&!sizes&&!bytes_equal(mode,L"emoji",sizeof(L"emoji")))return 2;
    IDWriteFactory3* factory=nullptr;IDWriteTextAnalyzer* analyzer=nullptr;IDWriteTextAnalyzer1* a1=nullptr;
    IDWriteFontSetBuilder* builder=nullptr;IDWriteFontSet* set=nullptr;IDWriteFontCollection1* collection=nullptr;IDWriteTextFormat* format=nullptr;IDWriteTextLayout* layout=nullptr;
    HRESULT hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory3),reinterpret_cast<IUnknown**>(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateTextAnalyzer(&analyzer);if(SUCCEEDED(hr))hr=analyzer->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&a1));
    if(SUCCEEDED(hr))hr=analyzer->AnalyzeScript(&analysis,0,analysis.length,&analysis);
    if(SUCCEEDED(hr))hr=factory->CreateFontSetBuilder(&builder);
    for(u32 i=0;i<2&&SUCCEEDED(hr);++i){IDWriteFontFaceReference* face=nullptr;hr=factory->CreateFontFaceReference(paths[i],nullptr,0,DWRITE_FONT_SIMULATIONS_NONE,&face);if(SUCCEEDED(hr))hr=builder->AddFontFaceReference(face);com_release(face);}
    if(SUCCEEDED(hr))hr=builder->CreateFontSet(&set);if(SUCCEEDED(hr))hr=factory->CreateFontCollectionFromFontSet(set,&collection);
    if(SUCCEEDED(hr))hr=factory->CreateTextFormat(L"MaiCord Cut Text",collection,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,18,L"en-US",&format);
    if(SUCCEEDED(hr))hr=format->SetReadingDirection(analysis.rtl?DWRITE_READING_DIRECTION_RIGHT_TO_LEFT:DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
    if(SUCCEEDED(hr))hr=factory->CreateTextLayout(analysis.text,analysis.length,format,1000000,1000000,&layout);
    char text[1024];const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,analysis.text,static_cast<int>(analysis.length),text,1024,nullptr,nullptr);if(!bytes)hr=E_INVALIDARG;
    if(SUCCEEDED(hr)&&whole)hr=layout->SetFontFamilyName(L"MaiCord Cut Emoji",{0,analysis.length});
    if(SUCCEEDED(hr)&&!whole){TextEmojiCursor cursor{};TextEmojiRange range{};
        for(;;){const auto next=text_emoji_next({text,static_cast<u32>(bytes)},&cursor,&range);if(next==TextEmojiNext::end)break;if(next==TextEmojiNext::invalid_input){hr=E_INVALIDARG;break;}
            hr=layout->SetFontFamilyName(L"MaiCord Cut Emoji",{range.first_unit,range.end_unit-range.first_unit});if(FAILED(hr))break;}}
    if(SUCCEEDED(hr)&&sizes){u32 unit=0;for(u32 at=0;at<static_cast<u32>(bytes);){const auto cp=utf8_decode_one(text+at,static_cast<u32>(bytes)-at);const u32 count=cp.codepoint>0xffff?2u:1u;
            hr=layout->SetFontSize(unit&1?21.5f:18.0f,{unit,count});if(FAILED(hr))break;at+=cp.width;unit+=count;}}
    field(utf8_literal("{\"setup_hresult\":"),static_cast<u32>(hr));print(utf8_literal(",\"scripts\":["));
    for(u32 first=0;first<analysis.length&&SUCCEEDED(hr);){const auto script=analysis.scripts[first];u32 end=first+1;while(end<analysis.length&&analysis.scripts[end].script==script.script&&analysis.scripts[end].shapes==script.shapes)++end;
        DWRITE_SCRIPT_PROPERTIES p{};hr=a1->GetScriptProperties(script,&p);if(FAILED(hr))break;if(first)print(utf8_literal(","));field(utf8_literal("["),first);field(utf8_literal(","),end-first);field(utf8_literal(","),script.script);field(utf8_literal(","),p.isoScriptCode);field(utf8_literal(","),script.shapes);print(utf8_literal("]"));first=end;}
    print(utf8_literal("],\"runs\":["));CutCapture capture;if(SUCCEEDED(hr))hr=layout->Draw(nullptr,&capture,0,0);field(utf8_literal("],\"hresult\":"),static_cast<u32>(hr));
    if(!module_path())output_ok=false;print(utf8_literal("}\n"));flush();
    com_release(layout);com_release(format);com_release(collection);com_release(set);com_release(builder);com_release(a1);com_release(analyzer);com_release(factory);return SUCCEEDED(hr)&&output_ok?0:1;
}
}}
extern "C" int maicord_script_cut_reference_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::cut_main()));}
