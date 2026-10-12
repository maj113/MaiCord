// QA only. Observe TextLayout bidi/geometry across a font-property range.
#define maicord_gsub_execution_reference_entry maicord_unused_shape_reference_entry
#include "gsub_execution_reference.cpp"
#undef maicord_gsub_execution_reference_entry
#include "maicord/ui/text_style.hpp"
namespace maicord {namespace {
struct StyleCapture final : IDWriteTextRenderer {
    bool comma=false,inline_draw=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** value) override {
        if(!value)return E_POINTER;*value=nullptr;
        if(!bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID))&&!bytes_equal(&iid,&__uuidof(IDWriteTextRenderer),sizeof(GUID))&&!bytes_equal(&iid,&__uuidof(IDWritePixelSnapping),sizeof(GUID)))return E_NOINTERFACE;
        *value=static_cast<IDWriteTextRenderer*>(this);return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;} ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* value) override {*value=TRUE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* value) override {*value={1,0,0,1,0,0};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value) override {*value=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION* description,IUnknown*) override {
        if(!run)return E_INVALIDARG;if(comma)print(utf8_literal(","));comma=true;
        field(utf8_literal("{\"first\":"),description?description->textPosition:0);field(utf8_literal(",\"length\":"),description?description->stringLength:0);field(utf8_literal(",\"bidi\":"),run->bidiLevel);
        field(utf8_literal(",\"inline\":"),inline_draw?1u:0u);
        print(utf8_literal(",\"origin_em_bits\":["));const float values[]{x,y,run->fontEmSize};for(u32 i=0;i<3;++i){u32 bits=0;copy_bytes(&bits,values+i,4);if(i)print(utf8_literal(","));number(bits);}
        print(utf8_literal("],\"glyphs\":["));for(u32 i=0;i<run->glyphCount;++i){if(i)print(utf8_literal(","));number(run->glyphIndices[i]);}
        print(utf8_literal("],\"advances\":["));for(u32 i=0;i<run->glyphCount;++i){u32 bits=0;copy_bytes(&bits,run->glyphAdvances+i,4);if(i)print(utf8_literal(","));number(bits);}
        print(utf8_literal("]}"));return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context,FLOAT x,FLOAT y,IDWriteInlineObject* object,BOOL sideways,BOOL rtl,IUnknown* effect) override {
        if(!object||inline_draw)return E_FAIL;inline_draw=true;const auto result=object->Draw(context,this,x,y,sideways,rtl,effect);inline_draw=false;return result;}
};
bool decimal_argument(const wchar_t** command,u32* value) noexcept {
    wchar_t encoded[32];if(!argument(command,encoded,32))return false;u32 number=0;
    for(u32 i=0;encoded[i];++i){if(encoded[i]<'0'||encoded[i]>'9'||number>(0xffffffffu-static_cast<u32>(encoded[i]-'0'))/10)return false;number=number*10+encoded[i]-'0';}
    *value=number;return true;
}
int bidi_style_main() noexcept {
    const wchar_t* command=GetCommandLineW();wchar_t executable[32768],encoded[4096],family[256];u32 args[6]{};
    if(!argument(&command,executable,32768)||!argument(&command,encoded,4096))return 2;
    for(auto& value:args)if(!decimal_argument(&command,&value))return 2;
    if(!argument(&command,family,256))return 2;while(*command==' '||*command=='\t')++command;
    float height=1000000;u32 trimming=0;
    if(*command){u32 height_bits=0;if(!decimal_argument(&command,&height_bits)||!decimal_argument(&command,&trimming)||trimming>1)return 2;
        copy_bytes(&height,&height_bits,4);if(!(height>0)||height>1000000)return 2;while(*command==' '||*command=='\t')++command;if(*command)return 2;}
    Analysis input;if(!parse_text(encoded,&input))return 2;
    const u32 first=args[0],count=args[1],flags=args[2];float width=0,em=0;copy_bytes(&width,args+3,4);copy_bytes(&em,args+4,4);
    if(first>input.length||count>input.length-first||!(width>0)||width>1000000||!(em>0)||em>1000||args[5]>1)return 2;
    IDWriteFactory* factory=nullptr;IDWriteTextFormat* format=nullptr;IDWriteTextLayout* layout=nullptr;
    auto hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format);
    if(SUCCEEDED(hr))hr=factory->CreateTextLayout(input.text,input.length,format,width,height,&layout);
    if(SUCCEEDED(hr)&&trimming){IDWriteInlineObject* sign=nullptr;hr=factory->CreateEllipsisTrimmingSign(layout,&sign);
        const DWRITE_TRIMMING option{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};if(SUCCEEDED(hr))hr=layout->SetTrimming(&option,sign);com_release(sign);}
    const DWRITE_TEXT_RANGE range{first,count};
    if(SUCCEEDED(hr)&&args[5])hr=layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if(SUCCEEDED(hr)&&(flags&text_style_bold))hr=layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,range);
    if(SUCCEEDED(hr)&&(flags&text_style_italic))hr=layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC,range);
    if(SUCCEEDED(hr)&&(flags&text_style_underline))hr=layout->SetUnderline(TRUE,range);
    if(SUCCEEDED(hr)&&(flags&text_style_strikethrough))hr=layout->SetStrikethrough(TRUE,range);
    if(SUCCEEDED(hr)&&(flags&text_style_monospace))hr=layout->SetFontFamilyName(L"Consolas",range);
    const float size=text_style_font_size(em,flags);if(SUCCEEDED(hr)&&size!=em)hr=layout->SetFontSize(size,range);
    print(utf8_literal("{\"positions\":["));
    for(u32 i=0;SUCCEEDED(hr)&&i<input.length;++i){float x=0,y=0;DWRITE_HIT_TEST_METRICS metric{};hr=layout->HitTestTextPosition(i,FALSE,&x,&y,&metric);if(FAILED(hr))break;
        if(i)print(utf8_literal(","));field(utf8_literal("["),metric.textPosition);field(utf8_literal(","),metric.length);field(utf8_literal(","),metric.bidiLevel);
        const float values[]{x,y,metric.left,metric.top,metric.width,metric.height};for(float value:values){u32 bits=0;copy_bytes(&bits,&value,4);field(utf8_literal(","),bits);}print(utf8_literal("]"));}
    print(utf8_literal("],\"runs\":["));StyleCapture capture;if(SUCCEEDED(hr))hr=layout->Draw(nullptr,&capture,0,0);
    print(utf8_literal("],\"metrics\":["));DWRITE_TEXT_METRICS metrics{};if(SUCCEEDED(hr))hr=layout->GetMetrics(&metrics);
    const float dimensions[]{metrics.width,metrics.widthIncludingTrailingWhitespace,metrics.height};for(u32 i=0;i<3;++i){if(i)print(utf8_literal(","));u32 bits=0;copy_bytes(&bits,dimensions+i,4);number(bits);}field(utf8_literal(","),metrics.lineCount);
    print(utf8_literal("],\"lines\":["));DWRITE_LINE_METRICS lines[256];u32 count_lines=0;if(SUCCEEDED(hr))hr=layout->GetLineMetrics(lines,256,&count_lines);
    for(u32 i=0;SUCCEEDED(hr)&&i<count_lines;++i){if(i)print(utf8_literal(","));const auto& line=lines[i];field(utf8_literal("["),line.length);field(utf8_literal(","),line.trailingWhitespaceLength);field(utf8_literal(","),line.newlineLength);
        const float values[]{line.height,line.baseline};for(float value:values){u32 bits=0;copy_bytes(&bits,&value,4);field(utf8_literal(","),bits);}field(utf8_literal(","),line.isTrimmed);print(utf8_literal("]"));}
    field(utf8_literal("],\"hresult\":"),static_cast<u32>(hr));if(!module_path())output_ok=false;print(utf8_literal("}\n"));flush();
    com_release(layout);com_release(format);com_release(factory);return SUCCEEDED(hr)&&output_ok?0:1;
}
}}
extern "C" int maicord_bidi_style_reference_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::bidi_style_main()));}
