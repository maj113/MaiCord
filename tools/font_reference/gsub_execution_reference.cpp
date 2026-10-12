// QA only. Measures installed DirectWrite; no native execution policy is assumed.
#include "maicord/core/slice.hpp"
#include <dwrite.h>
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
#include <dwrite_3.h>
#endif

namespace maicord { namespace {
constexpr u32 text_capacity=256,glyph_capacity=4096;
char output[65536];u32 output_size=0;bool output_ok=true;
void flush() noexcept {DWORD done=0;if(output_size && (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),output,output_size,&done,nullptr) || done!=output_size))output_ok=false;output_size=0;}
void print(Utf8View value) noexcept {for(u32 i=0;i<value.bytes;++i){if(output_size==sizeof(output))flush();output[output_size++]=value.data[i];}}
void number(u32 value) noexcept {char s[16];u32 n=0;do{s[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char t=s[i];s[i]=s[n-i-1];s[n-i-1]=t;}print({s,n});}
void field(Utf8View name,u32 value) noexcept {print(name);number(value);}
bool module_path() noexcept {
    wchar_t path[32768];const DWORD count=GetModuleFileNameW(GetModuleHandleW(L"dwrite.dll"),path,32768);
    if(!count || count>=32768)return false;
    char encoded[32768*3];const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,static_cast<int>(count),encoded,sizeof(encoded),nullptr,nullptr);
    if(!bytes)return false;print(utf8_literal(",\"dwrite_module\":\""));
    for(int i=0;i<bytes;++i){if(static_cast<u8>(encoded[i])<32)return false;if(encoded[i]=='\\' || encoded[i]=='"')print(utf8_literal("\\"));print({encoded+i,1});}
    print(utf8_literal("\""));return true;
}
struct Analysis final : IDWriteTextAnalysisSource,IDWriteTextAnalysisSink {
    wchar_t text[text_capacity]{};DWRITE_SCRIPT_ANALYSIS scripts[text_capacity]{};bool seen[text_capacity]{};u32 length=0;bool rtl=false;
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
    const wchar_t* locale=L"en-US";
#endif
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID)) || bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSource),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSource*>(this);
        else if(bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSink),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSink*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 at,WCHAR const** value,UINT32* count) override {*value=at<length?text+at:nullptr;*count=at<length?length-at:0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 at,WCHAR const** value,UINT32* count) override {if(at>length)return E_INVALIDARG;*value=at?text:nullptr;*count=at;return S_OK;}
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {return rtl?DWRITE_READING_DIRECTION_RIGHT_TO_LEFT:DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;}
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 at,UINT32* count,WCHAR const** value) override {if(at>length)return E_INVALIDARG;*count=length-at;
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
        *value=locale;
#else
        *value=L"en-US";
#endif
        return S_OK;}
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 at,UINT32* count,IDWriteNumberSubstitution** value) override {if(at>length)return E_INVALIDARG;*count=length-at;*value=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32 at,UINT32 count,DWRITE_SCRIPT_ANALYSIS const* value) override {
        if(at>length || count>length-at || !value)return E_INVALIDARG;
        for(u32 i=at;i<at+count;++i){if(seen[i])return E_UNEXPECTED;scripts[i]=*value;seen[i]=true;}return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32,UINT32,DWRITE_LINE_BREAKPOINT const*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32,UINT32,IDWriteNumberSubstitution*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32,UINT32,UINT8,UINT8) override {return E_NOTIMPL;}
};
bool argument(const wchar_t** command,wchar_t* out,u32 capacity) noexcept {
    const wchar_t* p=*command;while(*p==' ' || *p=='\t')++p;
    const bool quoted=*p=='"';if(quoted)++p;u32 n=0;
    while(*p && !(quoted?*p=='"':(*p==' ' || *p=='\t'))){if(n+1>=capacity)return false;out[n++]=*p++;}
    if(quoted){if(*p!='"')return false;++p;}out[n]=0;*command=p;return n!=0;
}
bool parse_text(const wchar_t* value,Analysis* analysis) noexcept {
    while(*value){u32 cp=0,digits=0;
        while(*value && *value!=','){const wchar_t c=*value++;u32 digit=16;if(c>='0' && c<='9')digit=c-'0';else if(c>='a' && c<='f')digit=c-'a'+10;else if(c>='A' && c<='F')digit=c-'A'+10;
            if(digit==16 || ++digits>6)return false;cp=cp*16+digit;}
        if(!digits || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))return false;
        const u32 units=cp>0xffff?2u:1u;if(units>text_capacity-analysis->length)return false;
        if(units==1)analysis->text[analysis->length++]=static_cast<wchar_t>(cp);
        else{cp-=0x10000;analysis->text[analysis->length++]=static_cast<wchar_t>(0xd800+(cp>>10));analysis->text[analysis->length++]=static_cast<wchar_t>(0xdc00+(cp&1023));}
        if(*value){++value;if(!*value)return false;}
    }return analysis->length!=0;
}
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
#if defined(MAICORD_FORCED_FACE_REFERENCE)
// QA-only owned-face geometry. App font fallback is tested separately.
struct ForcedFaceFallback final : IDWriteFontFallback {
    IDWriteFont* font=nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;if(!bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID))&&!bytes_equal(&iid,&__uuidof(IDWriteFontFallback),sizeof(GUID)))return E_NOINTERFACE;
        *out=static_cast<IDWriteFontFallback*>(this);return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE MapCharacters(IDWriteTextAnalysisSource*,UINT32,UINT32 length,IDWriteFontCollection*,const wchar_t*,DWRITE_FONT_WEIGHT,DWRITE_FONT_STYLE,DWRITE_FONT_STRETCH,
        UINT32* mapped,IDWriteFont** selected,FLOAT* scale) override {
        if(!font||!mapped||!selected||!scale)return E_INVALIDARG;*mapped=length;*selected=font;*scale=1;font->AddRef();return S_OK;
    }
};
#endif
HRESULT font_weight_bits(IDWriteFontFace* face,u32* result) noexcept {
    IDWriteFontFace5* f5=nullptr;auto hr=face->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&f5));DWRITE_FONT_AXIS_VALUE values[32]{};bool found=false;
    if(SUCCEEDED(hr)){const u32 count=f5->GetFontAxisValueCount();if(count>32)hr=E_FAIL;else{hr=f5->GetFontAxisValues(values,count);if(SUCCEEDED(hr))for(u32 i=0;i<count;++i)if(values[i].axisTag==DWRITE_FONT_AXIS_TAG_WEIGHT){copy_bytes(result,&values[i].value,4);found=true;}}}
    com_release(f5);return FAILED(hr)?hr:found?S_OK:E_UNEXPECTED;
}
HRESULT metric_observation(IDWriteFontFace* face,u32 weight,bool include_advances=true) noexcept {
    IDWriteFontFace1* f1=nullptr;HRESULT hr=face->QueryInterface(__uuidof(IDWriteFontFace1),reinterpret_cast<void**>(&f1));u32 bits=0;
    if(SUCCEEDED(hr))hr=font_weight_bits(face,&bits);
    field(utf8_literal("{\"requested_weight\":"),weight);field(utf8_literal(",\"face_weight_bits\":"),bits);if(!module_path())output_ok=false;
    DWRITE_FONT_METRICS metrics{};face->GetMetrics(&metrics);
    const i32 fields[]{metrics.designUnitsPerEm,metrics.ascent,metrics.descent,metrics.lineGap,metrics.capHeight,metrics.xHeight,metrics.underlinePosition,metrics.underlineThickness,metrics.strikethroughPosition,metrics.strikethroughThickness};
    print(utf8_literal(",\"font_metrics\":["));for(u32 i=0;i<10;++i){if(i)print(utf8_literal(","));if(fields[i]<0)print(utf8_literal("-"));number(static_cast<u32>(fields[i]<0?-fields[i]:fields[i]));}print(utf8_literal("],\"advances\":["));
    const u32 count=include_advances?face->GetGlyphCount():0;u16 ids[256];i32 advances[256];
    for(u32 first=0;first<count && SUCCEEDED(hr);first+=256){const u32 n=minimum(256u,count-first);for(u32 i=0;i<n;++i)ids[i]=static_cast<u16>(first+i);hr=f1->GetDesignGlyphAdvances(n,ids,advances,FALSE);
        if(SUCCEEDED(hr))for(u32 i=0;i<n;++i){if(first+i)print(utf8_literal(","));if(advances[i]<0)print(utf8_literal("-"));number(static_cast<u32>(advances[i]<0?-static_cast<i64>(advances[i]):advances[i]));}}
    DWRITE_FONT_METRICS1 extended{};if(SUCCEEDED(hr))f1->GetMetrics(&extended);
    field(utf8_literal("],\"has_typographic_metrics\":"),extended.hasTypographicMetrics);
    field(utf8_literal(",\"hresult\":"),static_cast<u32>(hr));print(utf8_literal("}\n"));com_release(f1);return hr;
}
struct LayoutCapture final : IDWriteTextRenderer {
#if defined(MAICORD_ITEMIZED_LAYOUT_REFERENCE)
    u32 run_data[text_capacity][7]{};u32 run_count=0;
    HRESULT item_status=S_OK;
#endif
#if defined(MAICORD_FORCED_FACE_REFERENCE)
    IDWriteFontFace5* expected_face=nullptr;
    HRESULT face_status=S_OK;
    u32 verified_face_runs=0;
#endif
    u16 glyphs[glyph_capacity]{};float advances[glyph_capacity]{};DWRITE_GLYPH_OFFSET offsets[glyph_capacity]{};u32 count=0,bidi=0;
    u32 clusters[text_capacity]{};bool cluster_seen[text_capacity]{};
    bool capture_weight=false;u32 weight_bits=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID)) &&
           !bytes_equal(&iid,&__uuidof(IDWritePixelSnapping),sizeof(GUID)) &&
           !bytes_equal(&iid,&__uuidof(IDWriteTextRenderer),sizeof(GUID)))return E_NOINTERFACE;
        *out=static_cast<IDWriteTextRenderer*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* value) override {*value=TRUE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* value) override {*value={1,0,0,1,0,0};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value) override {*value=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION* description,IUnknown*) override {
#if defined(MAICORD_ITEMIZED_LAYOUT_REFERENCE)
        if(!run||!description||run_count==text_capacity||run->glyphCount>glyph_capacity-count||description->textPosition>text_capacity||description->stringLength>text_capacity-description->textPosition||
           (run->glyphCount&&(!description->clusterMap||!run->glyphIndices||!run->glyphAdvances)))return item_status=E_UNEXPECTED;
        auto* r=run_data[run_count++];r[0]=description->textPosition;r[1]=description->stringLength;r[2]=count;r[3]=run->glyphCount;r[4]=run->bidiLevel;copy_bytes(r+5,&x,4);copy_bytes(r+6,&y,4);
#else
        (void)x;(void)y;
#endif
        if(!run || run->glyphCount>glyph_capacity-count)return E_FAIL;bidi=run->bidiLevel;
#if defined(MAICORD_FORCED_FACE_REFERENCE)
        if(!expected_face||!expected_face->Equals(run->fontFace)){
            face_status=E_UNEXPECTED;
            field(utf8_literal(",\"expected_simulations\":"),expected_face?expected_face->GetSimulations():0xffffffffu);
            field(utf8_literal(",\"actual_simulations\":"),run->fontFace->GetSimulations());
            field(utf8_literal(",\"actual_glyph_count\":"),run->fontFace->GetGlyphCount());
            field(utf8_literal(",\"expected_glyph_count\":"),expected_face?expected_face->GetGlyphCount():0);
            IDWriteFontFace5* observed=nullptr;
            if(SUCCEEDED(run->fontFace->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&observed)))){
                print(utf8_literal(",\"mismatch_axes\":["));IDWriteFontFace5* faces[]{expected_face,observed};
                for(u32 f=0;f<2;++f){if(f)print(utf8_literal(","));print(utf8_literal("["));DWRITE_FONT_AXIS_VALUE axes[16]{};
                    const u32 n=faces[f]?faces[f]->GetFontAxisValueCount():0;if(n<=16&&faces[f]&&SUCCEEDED(faces[f]->GetFontAxisValues(axes,n)))for(u32 i=0;i<n;++i){if(i)print(utf8_literal(","));u32 bits=0;copy_bytes(&bits,&axes[i].value,4);field(utf8_literal("["),axes[i].axisTag);field(utf8_literal(","),bits);print(utf8_literal("]"));}print(utf8_literal("]"));}
                print(utf8_literal("]"));com_release(observed);
            }
            return face_status;
        }
        ++verified_face_runs;
#endif
#if defined(MAICORD_ITEMIZED_LAYOUT_REFERENCE)
        for(u32 i=0;i<description->stringLength;++i){const u32 at=description->textPosition+i;
            if(cluster_seen[at]||(run->glyphCount&&description->clusterMap[i]>run->glyphCount))return item_status=E_UNEXPECTED;
            cluster_seen[at]=true;clusters[at]=run->glyphCount&&description->clusterMap[i]<run->glyphCount?count+description->clusterMap[i]:0xffffffffu;}
#else
        if(!description||description->textPosition>text_capacity||description->stringLength>text_capacity-description->textPosition||!description->clusterMap)return E_FAIL;
        for(u32 i=0;i<description->stringLength;++i){const u32 at=description->textPosition+i;if(cluster_seen[at]||description->clusterMap[i]>=run->glyphCount)return E_UNEXPECTED;cluster_seen[at]=true;clusters[at]=count+description->clusterMap[i];}
#endif
        if(capture_weight){u32 bits=0;const auto hr=font_weight_bits(run->fontFace,&bits);if(FAILED(hr))return hr;if(count && weight_bits!=bits)return E_UNEXPECTED;weight_bits=bits;}
        for(u32 i=0;i<run->glyphCount;++i){glyphs[count]=run->glyphIndices[i];advances[count]=run->glyphAdvances?run->glyphAdvances[i]:0;offsets[count]=run->glyphOffsets?run->glyphOffsets[i]:DWRITE_GLYPH_OFFSET{};++count;}return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override {return E_NOTIMPL;}
};
void hit_fields(const DWRITE_HIT_TEST_METRICS& m) noexcept {
    field(utf8_literal("["),m.textPosition);field(utf8_literal(","),m.length);
    const float fields[]{m.left,m.top,m.width,m.height};for(float value:fields){u32 bits=0;copy_bytes(&bits,&value,4);field(utf8_literal(","),bits);}
    field(utf8_literal(","),m.bidiLevel);field(utf8_literal(","),m.isText);field(utf8_literal(","),m.isTrimmed);print(utf8_literal("]"));
}
HRESULT point_observation(IDWriteTextLayout* layout,float x,float y,u32* count) noexcept {
    BOOL trailing=FALSE,inside=FALSE;DWRITE_HIT_TEST_METRICS metric{};const auto hr=layout->HitTestPoint(x,y,&trailing,&inside,&metric);if(FAILED(hr))return hr;
    if((*count)++)print(utf8_literal(","));u32 xb=0,yb=0;copy_bytes(&xb,&x,4);copy_bytes(&yb,&y,4);
    field(utf8_literal("["),xb);field(utf8_literal(","),yb);field(utf8_literal(","),trailing);field(utf8_literal(","),inside);print(utf8_literal(","));hit_fields(metric);print(utf8_literal("]"));return hr;
}
HRESULT layout_observation(IDWriteFactory* factory,IDWriteFontFace* face,const Analysis& analysis,float em,DWRITE_FONT_FEATURE* features,u32 feature_count,u32 weight,bool hit_layout=false,bool edit_layout=false) noexcept {
    IDWriteFactory3* f3=nullptr;IDWriteFontFace3* face3=nullptr;IDWriteFontFaceReference* reference=nullptr;IDWriteFontSetBuilder* builder=nullptr;IDWriteFontSet* set=nullptr;IDWriteFontCollection1* collection=nullptr;
    IDWriteLocalizedStrings* names=nullptr;IDWriteTextFormat* format=nullptr;IDWriteTextLayout* layout=nullptr;IDWriteTypography* typography=nullptr;wchar_t family[256]{};
    HRESULT hr=factory->QueryInterface(__uuidof(IDWriteFactory3),reinterpret_cast<void**>(&f3));if(SUCCEEDED(hr))hr=face->QueryInterface(__uuidof(IDWriteFontFace3),reinterpret_cast<void**>(&face3));
    if(SUCCEEDED(hr))hr=face3->GetFontFaceReference(&reference);if(SUCCEEDED(hr))hr=f3->CreateFontSetBuilder(&builder);
#if defined(MAICORD_FORCED_FACE_REFERENCE)
    // The collection describes this concrete variable instance. Its source
    // OS/2 default weight would otherwise request an additional bold simulation.
    if(SUCCEEDED(hr)&&weight){wchar_t value[12]{};u32 copy=weight,n=0;do{value[n++]=static_cast<wchar_t>('0'+copy%10);copy/=10;}while(copy);
        for(u32 i=0;i<n/2;++i){const auto c=value[i];value[i]=value[n-i-1];value[n-i-1]=c;}
        const DWRITE_FONT_PROPERTY property{DWRITE_FONT_PROPERTY_ID_WEIGHT,value,nullptr};hr=builder->AddFontFaceReference(reference,&property,1);
    }else if(SUCCEEDED(hr))hr=builder->AddFontFaceReference(reference);
#else
    if(SUCCEEDED(hr))hr=builder->AddFontFaceReference(reference);
#endif
    if(SUCCEEDED(hr))hr=builder->CreateFontSet(&set);if(SUCCEEDED(hr))hr=f3->CreateFontCollectionFromFontSet(set,&collection);
    if(SUCCEEDED(hr))hr=face3->GetFamilyNames(&names);if(SUCCEEDED(hr))hr=names->GetString(0,family,256);if(SUCCEEDED(hr))hr=factory->CreateTextFormat(family,collection,static_cast<DWRITE_FONT_WEIGHT>(weight?weight:400),DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,analysis.locale,&format);
    if(SUCCEEDED(hr))hr=factory->CreateTextLayout(analysis.text,analysis.length,format,1000000,1000000,&layout);
#if defined(MAICORD_ITEMIZED_LAYOUT_REFERENCE)
    // Match an explicit app font override, including non-visual source items.
    if(SUCCEEDED(hr))hr=layout->SetFontCollection(collection,{0,analysis.length});
    if(SUCCEEDED(hr))hr=layout->SetFontFamilyName(family,{0,analysis.length});
#endif
#if defined(MAICORD_FORCED_FACE_REFERENCE)
    ForcedFaceFallback fallback;IDWriteFontFamily* selected_family=nullptr;IDWriteTextLayout2* layout2=nullptr;IDWriteFontFace5* expected_face=nullptr;
    if(SUCCEEDED(hr))hr=collection->GetFontFamily(0,&selected_family);
    if(SUCCEEDED(hr))hr=selected_family->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(weight?weight:400),DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,&fallback.font);
    if(SUCCEEDED(hr))hr=layout->QueryInterface(__uuidof(IDWriteTextLayout2),reinterpret_cast<void**>(&layout2));
    if(SUCCEEDED(hr))hr=layout2->SetFontFallback(&fallback);
    if(SUCCEEDED(hr))hr=face->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&expected_face));
    com_release(layout2);com_release(selected_family);
#endif
    if(SUCCEEDED(hr) && feature_count){hr=factory->CreateTypography(&typography);for(u32 i=0;i<feature_count && SUCCEEDED(hr);++i)hr=typography->AddFontFeature(features[i]);if(SUCCEEDED(hr))hr=layout->SetTypography(typography,{0,analysis.length});}
    LayoutCapture capture;capture.capture_weight=weight!=0;
#if defined(MAICORD_FORCED_FACE_REFERENCE)
    capture.expected_face=expected_face;print(utf8_literal(",\"forced_face\":1"));
#endif
    if(SUCCEEDED(hr))hr=layout->Draw(nullptr,&capture,0,0);
#if defined(MAICORD_ITEMIZED_LAYOUT_REFERENCE)
    if(SUCCEEDED(hr)&&FAILED(capture.item_status))hr=capture.item_status;
    if(SUCCEEDED(hr))for(u32 i=0;i<analysis.length;++i)if(!capture.cluster_seen[i])hr=E_UNEXPECTED;
    print(utf8_literal(",\"layout_runs\":["));for(u32 i=0;i<capture.run_count;++i){if(i)print(utf8_literal(","));print(utf8_literal("["));
        for(u32 k=0;k<7;++k){if(k)print(utf8_literal(","));number(capture.run_data[i][k]);}print(utf8_literal("]"));}print(utf8_literal("]"));
#endif
#if defined(MAICORD_FORCED_FACE_REFERENCE)
    if(SUCCEEDED(hr)&&FAILED(capture.face_status))hr=capture.face_status;
    field(utf8_literal(",\"verified_face_runs\":"),capture.verified_face_runs);
#endif
    if(weight){u32 raw_weight=0;const auto weight_hr=font_weight_bits(face,&raw_weight);if(FAILED(weight_hr))hr=weight_hr;field(utf8_literal(",\"requested_weight\":"),weight);field(utf8_literal(",\"face_weight_bits\":"),raw_weight);field(utf8_literal(",\"layout_weight_bits\":"),capture.weight_bits);}
    field(utf8_literal(",\"layout_hresult\":"),static_cast<u32>(hr));field(utf8_literal(",\"layout_bidi\":"),capture.bidi);print(utf8_literal(",\"layout_glyphs\":["));for(u32 i=0;i<capture.count;++i){if(i)print(utf8_literal(","));number(capture.glyphs[i]);}print(utf8_literal("],\"layout_placement_bits\":["));
    for(u32 i=0;i<capture.count;++i){u32 a=0,x=0,y=0;copy_bytes(&a,&capture.advances[i],4);copy_bytes(&x,&capture.offsets[i].advanceOffset,4);copy_bytes(&y,&capture.offsets[i].ascenderOffset,4);if(i)print(utf8_literal(","));field(utf8_literal("["),a);field(utf8_literal(","),x);field(utf8_literal(","),y);print(utf8_literal("]"));}print(utf8_literal("]"));
    print(utf8_literal(",\"layout_clusters\":["));for(u32 i=0;i<analysis.length;++i){if(i)print(utf8_literal(","));number(capture.cluster_seen[i]?capture.clusters[i]:0xffffffffu);}print(utf8_literal("]"));
    DWRITE_LINE_METRICS lines[text_capacity]{};UINT32 line_count=0;DWRITE_TEXT_METRICS text_metrics{};
    if(SUCCEEDED(hr))hr=layout->GetLineMetrics(lines,text_capacity,&line_count);if(SUCCEEDED(hr))hr=layout->GetMetrics(&text_metrics);
    print(utf8_literal(",\"layout_lines\":["));if(SUCCEEDED(hr))for(u32 i=0;i<line_count;++i){u32 height=0,baseline=0;copy_bytes(&height,&lines[i].height,4);copy_bytes(&baseline,&lines[i].baseline,4);if(i)print(utf8_literal(","));field(utf8_literal("["),lines[i].length);field(utf8_literal(","),lines[i].trailingWhitespaceLength);field(utf8_literal(","),lines[i].newlineLength);field(utf8_literal(","),height);field(utf8_literal(","),baseline);print(utf8_literal("]"));}
    print(utf8_literal("],\"layout_metrics_bits\":["));const float dimensions[]{text_metrics.width,text_metrics.widthIncludingTrailingWhitespace,text_metrics.height};for(u32 i=0;i<3;++i){u32 bits=0;copy_bytes(&bits,dimensions+i,4);if(i)print(utf8_literal(","));number(bits);}field(utf8_literal("],\"line_metrics_hresult\":"),static_cast<u32>(hr));
    if(hit_layout){
        DWRITE_CLUSTER_METRICS clusters[text_capacity]{};UINT32 count=0;if(SUCCEEDED(hr))hr=layout->GetClusterMetrics(clusters,text_capacity,&count);
        print(utf8_literal(",\"cluster_metrics\":["));if(SUCCEEDED(hr))for(u32 i=0;i<count;++i){const auto& c=clusters[i];u32 width=0;copy_bytes(&width,&c.width,4);if(i)print(utf8_literal(","));field(utf8_literal("["),c.length);field(utf8_literal(","),width);field(utf8_literal(","),c.canWrapLineAfter);field(utf8_literal(","),c.isWhitespace);field(utf8_literal(","),c.isNewline);field(utf8_literal(","),c.isSoftHyphen);field(utf8_literal(","),c.isRightToLeft);print(utf8_literal("]"));}
        print(utf8_literal("],\"position_hits\":["));
        for(u32 at=0;at<=analysis.length&&SUCCEEDED(hr);++at)for(u32 trailing=0;trailing<2&&SUCCEEDED(hr);++trailing){float x=0,y=0;DWRITE_HIT_TEST_METRICS metric{};hr=layout->HitTestTextPosition(at,trailing,&x,&y,&metric);if(FAILED(hr))break;
            if(at||trailing)print(utf8_literal(","));field(utf8_literal("["),at);field(utf8_literal(","),trailing);field(utf8_literal(","),metric.textPosition);field(utf8_literal(","),metric.length);
            const float values[]{x,y,metric.left,metric.top,metric.width,metric.height};for(float value:values){u32 bits=0;copy_bytes(&bits,&value,4);field(utf8_literal(","),bits);}field(utf8_literal(","),metric.bidiLevel);field(utf8_literal(","),metric.isText);field(utf8_literal(","),metric.isTrimmed);print(utf8_literal("]"));}
        field(utf8_literal("],\"hit_hresult\":"),static_cast<u32>(hr));
        if(edit_layout){
            print(utf8_literal(",\"point_hits\":["));u32 emitted=0,source=0;
            for(u32 i=0;i<count&&SUCCEEDED(hr);++i){float x=0,y=0;DWRITE_HIT_TEST_METRICS metric{};hr=layout->HitTestTextPosition(source,FALSE,&x,&y,&metric);if(FAILED(hr))break;
                const float mid=metric.left+metric.width*.5f;u32 mid_bits=0;copy_bytes(&mid_bits,&mid,4);const u32 before_bits=mid_bits?mid_bits-1:0,after_bits=mid_bits+1;float before=0,after=0;copy_bytes(&before,&before_bits,4);copy_bytes(&after,&after_bits,4);
                const float xs[]{metric.left-.01f,metric.left,metric.left+metric.width*.25f,before,mid,after,metric.left+metric.width*.75f,metric.left+metric.width,metric.left+metric.width+.01f};
                for(float px:xs){hr=point_observation(layout,px,metric.height*.5f,&emitted);if(FAILED(hr))break;}source+=clusters[i].length;
            }
            const float xs[]{-1,0,text_metrics.widthIncludingTrailingWhitespace*.25f,text_metrics.widthIncludingTrailingWhitespace,text_metrics.widthIncludingTrailingWhitespace+1};
            const float ys[]{-1,0,text_metrics.height*.5f,text_metrics.height,text_metrics.height+1};
            for(float x:xs)for(float y:ys)if(SUCCEEDED(hr))hr=point_observation(layout,x,y,&emitted);
            print(utf8_literal("],\"range_hits\":["));emitted=0;
            for(u32 at=0;at<=analysis.length&&SUCCEEDED(hr);++at){const u32 remaining=analysis.length-at, lengths[]{0,1,2,remaining};
                for(u32 k=0;k<4&&SUCCEEDED(hr);++k){const u32 length=lengths[k];if(length>remaining)continue;bool duplicate=false;for(u32 j=0;j<k;++j)duplicate=duplicate||lengths[j]==length;if(duplicate)continue;
                    DWRITE_HIT_TEST_METRICS rectangles[16]{};UINT32 actual=0;
                    // MaiCord's range wrapper returns no rectangles for empty selections.
                    if(length)hr=layout->HitTestTextRange(at,length,0,0,rectangles,16,&actual);if(FAILED(hr))break;
                    if(emitted++)print(utf8_literal(","));field(utf8_literal("["),at);field(utf8_literal(","),length);print(utf8_literal(",["));for(u32 j=0;j<actual;++j){if(j)print(utf8_literal(","));hit_fields(rectangles[j]);}print(utf8_literal("]]"));}
            }
            field(utf8_literal("],\"edit_hresult\":"),static_cast<u32>(hr));
        }
    }
    com_release(typography);com_release(layout);
#if defined(MAICORD_FORCED_FACE_REFERENCE)
    com_release(expected_face);com_release(fallback.font);
#endif
    com_release(format);com_release(names);com_release(collection);com_release(set);com_release(builder);com_release(reference);com_release(face3);com_release(f3);return hr;
}
HRESULT bmp_complexity(IDWriteTextAnalyzer* analyzer,IDWriteFontFace* face) noexcept {
    IDWriteTextAnalyzer1* a1=nullptr;HRESULT hr=analyzer->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&a1));u32 words[2048]{},mapped=0,simple_count=0;UINT16 baseline=0;const UINT32 a='a';
    if(SUCCEEDED(hr))hr=face->GetGlyphIndices(&a,1,&baseline);if(SUCCEEDED(hr) && !baseline)hr=E_UNEXPECTED;
    for(UINT32 cp=0;cp<65536 && SUCCEEDED(hr);++cp){if(cp>=0xd800 && cp<=0xdfff)continue;
        UINT16 glyph=0;hr=face->GetGlyphIndices(&cp,1,&glyph);if(FAILED(hr))break;if(glyph!=baseline){hr=E_UNEXPECTED;break;}++mapped;
        const WCHAR text=static_cast<WCHAR>(cp);BOOL simple=FALSE;UINT32 read=0;UINT16 nominal=0;
        hr=a1->GetTextComplexity(&text,1,face,&simple,&read,&nominal);if(FAILED(hr))break;if(read!=1 || (simple && nominal!=baseline)){hr=E_UNEXPECTED;break;}
        if(simple){words[cp/32]|=1u<<(cp%32);++simple_count;}
    }
    field(utf8_literal("{\"hresult\":"),static_cast<u32>(hr));field(utf8_literal(",\"mapped_scalars\":"),mapped);field(utf8_literal(",\"simple_scalars\":"),simple_count);field(utf8_literal(",\"glyph\":"),baseline);if(!module_path())output_ok=false;print(utf8_literal(",\"simple_words\":["));
    for(u32 i=0;i<2048;++i){if(i)print(utf8_literal(","));number(words[i]);}print(utf8_literal("]}\n"));com_release(a1);return hr;
}
HRESULT complexity_ranges(IDWriteTextAnalyzer* analyzer,IDWriteFontFace* face,const Analysis& text) noexcept {
    IDWriteTextAnalyzer1* a1=nullptr;HRESULT hr=analyzer->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&a1));
    print(utf8_literal("{\"ranges\":["));u32 at=0;
    while(SUCCEEDED(hr)&&at<text.length){BOOL simple=FALSE;UINT32 length=0;UINT16 glyphs[text_capacity]{};
        hr=a1->GetTextComplexity(text.text+at,text.length-at,face,&simple,&length,glyphs);
        if(FAILED(hr))break;if(!length||length>text.length-at){hr=E_UNEXPECTED;break;}
        if(at)print(utf8_literal(","));field(utf8_literal("["),at);field(utf8_literal(","),length);field(utf8_literal(","),simple);print(utf8_literal(",["));
        if(simple)for(u32 i=0;i<length;++i){if(i)print(utf8_literal(","));number(glyphs[i]);}print(utf8_literal("]]"));at+=length;
    }
    print(utf8_literal("]"));field(utf8_literal(",\"hresult\":"),static_cast<u32>(hr));field(utf8_literal(",\"units\":"),text.length);
    if(!module_path())output_ok=false;print(utf8_literal("}\n"));com_release(a1);return hr;
}
HRESULT complexity_profile(IDWriteTextAnalyzer* analyzer,IDWriteFontFace* face,bool interior) noexcept {
    IDWriteTextAnalyzer1* a1=nullptr;HRESULT hr=analyzer->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&a1));
    u32 start=0,previous=0xffffffffu,records=0;print(utf8_literal("{\"profile\":["));
    const auto emit=[&](u32 end) noexcept {if(records++)print(utf8_literal(","));field(utf8_literal("["),start);field(utf8_literal(","),end);field(utf8_literal(","),previous);print(utf8_literal("]"));};
    for(u32 cp=0;cp<0x110000&&SUCCEEDED(hr);++cp){u32 code=0;
        if(cp<0xd800||cp>0xdfff){WCHAR text[5]{L'a'};u32 n=1;if(interior)text[n++]=L'b';if(cp<65536)text[n++]=static_cast<WCHAR>(cp);else{const u32 v=cp-65536;text[n++]=static_cast<WCHAR>(0xd800+(v>>10));text[n++]=static_cast<WCHAR>(0xdc00+(v&1023));}text[n++]=L'b';
            BOOL simple=FALSE;UINT32 read=0;hr=a1->GetTextComplexity(text,n,face,&simple,&read,nullptr);if(FAILED(hr))break;if(!read||read>n){hr=E_UNEXPECTED;break;}code=read|(simple?128u:0);}
        if(previous!=code){if(previous!=0xffffffffu)emit(cp);start=cp;previous=code;}
    }
    if(SUCCEEDED(hr))emit(0x110000);print(utf8_literal("]"));field(utf8_literal(",\"hresult\":"),static_cast<u32>(hr));if(!module_path())output_ok=false;print(utf8_literal("}\n"));com_release(a1);return hr;
}
#endif
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
#include "shape_batch_reference.inc"
#include "metric_cost.inc"
HRESULT metric_throughput(IDWriteFontFace* face) noexcept {
    IDWriteFontFace1* f=nullptr;auto hr=face->QueryInterface(__uuidof(IDWriteFontFace1),reinterpret_cast<void**>(&f));if(FAILED(hr))return hr;
    i32 advances[256];
    const bool okay=metric_cost(face->GetGlyphCount(),[&](const u16* ids,u32 count) noexcept {return SUCCEEDED(f->GetDesignGlyphAdvances(count,ids,advances,FALSE));},
        [&](u32 i) noexcept {return static_cast<i64>(advances[i]);},[](const char* s) noexcept {u32 n=0;while(s[n])++n;print({s,n});},
        [](u64 value) noexcept {char s[21];u32 at=21;do{s[--at]=static_cast<char>('0'+value%10);value/=10;}while(value);print({s+at,21-at});});
    com_release(f);return okay?S_OK:E_FAIL;
}
#endif
int run() noexcept {
    const wchar_t* command=GetCommandLineW();wchar_t path[32768],codepoints[2048],enabled[512]{};
    if(!argument(&command,path,32768) || !argument(&command,path,32768) || !argument(&command,codepoints,2048) || !argument(&command,enabled,512))return 2;
#ifndef MAICORD_FEATURE_PLAN_REFERENCE
    if(enabled[1] || (enabled[0]!='0' && enabled[0]!='1'))return 2;
#endif
    bool rtl=false,sideways=false;
#ifdef MAICORD_PLACEMENT_REFERENCE
    wchar_t em_argument[16],rtl_argument[4],sideways_argument[4];u32 em_bits=0;
    if(!argument(&command,em_argument,16) || !argument(&command,rtl_argument,4) || !argument(&command,sideways_argument,4))return 2;
    for(u32 i=0;em_argument[i];++i){const wchar_t c=em_argument[i];if(c<'0' || c>'9' || em_bits>(0xffffffffu-static_cast<u32>(c-'0'))/10)return 2;em_bits=em_bits*10+static_cast<u32>(c-'0');}
    if(!em_bits || em_bits>=0x7f800000u || rtl_argument[1] || (rtl_argument[0]!='0' && rtl_argument[0]!='1') || sideways_argument[1] || (sideways_argument[0]!='0' && sideways_argument[0]!='1'))return 2;
    rtl=rtl_argument[0]=='1';sideways=sideways_argument[0]=='1';float em_size=0;copy_bytes(&em_size,&em_bits,4);
#endif
    const wchar_t* locale=L"en-US";
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
    wchar_t locale_argument[128];if(!argument(&command,locale_argument,128))return 2;locale=locale_argument;
    while(*command==' ' || *command=='\t')++command;bool measure_layout=false,measure_hit_layout=false,measure_edit_layout=false,measure_bmp=false,measure_metrics=false,measure_metric_cost=false,measure_font_metrics=false,measure_ranges=false,measure_profile=false,measure_interior=false;u32 selected_weight=0;
    if(*command){wchar_t mode[16]{};if(!argument(&command,mode,16))return 2;measure_edit_layout=bytes_equal(mode,L"edit-layout",sizeof(L"edit-layout"));measure_hit_layout=measure_edit_layout||bytes_equal(mode,L"hit-layout",sizeof(L"hit-layout"));measure_layout=measure_hit_layout||bytes_equal(mode,L"layout",sizeof(L"layout"));measure_bmp=bytes_equal(mode,L"bmp-complexity",sizeof(L"bmp-complexity"));measure_font_metrics=bytes_equal(mode,L"font-metrics",sizeof(L"font-metrics"));measure_metric_cost=bytes_equal(mode,L"metric-cost",sizeof(L"metric-cost"));measure_metrics=measure_metric_cost||measure_font_metrics||bytes_equal(mode,L"metrics",sizeof(L"metrics"));measure_ranges=bytes_equal(mode,L"ranges",sizeof(L"ranges"));measure_profile=bytes_equal(mode,L"range-profile",sizeof(L"range-profile"));measure_interior=bytes_equal(mode,L"range-interior",sizeof(L"range-interior"));if(!measure_layout && !measure_bmp && !measure_metrics&&!measure_ranges&&!measure_profile&&!measure_interior)return 2;}
    while(*command==' ' || *command=='\t')++command;
    if(*command){wchar_t weight[8]{};if((!measure_layout && !measure_metrics&&!measure_ranges) || !argument(&command,weight,8))return 2;for(u32 i=0;weight[i];++i){if(weight[i]<'0'||weight[i]>'9'||selected_weight>100)return 2;selected_weight=selected_weight*10+weight[i]-'0';}if(!selected_weight||selected_weight>1000)return 2;}
    if(measure_metrics && ((!selected_weight&&!measure_font_metrics) || rtl || sideways || !bytes_equal(enabled,L"default",sizeof(L"default"))))return 2;
#endif
    while(*command==' ' || *command=='\t')++command;if(*command)return 2;
    Analysis analysis;analysis.rtl=rtl;if(!parse_text(codepoints,&analysis))return 2;
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
    analysis.locale=locale;
#endif
    IDWriteFactory* factory=nullptr;IDWriteTextAnalyzer* analyzer=nullptr;IDWriteFontFile* file=nullptr;IDWriteFontFace* face=nullptr;
    HRESULT hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateTextAnalyzer(&analyzer);
    if(SUCCEEDED(hr))hr=factory->CreateFontFileReference(path,nullptr,&file);
    BOOL supported=FALSE;DWRITE_FONT_FILE_TYPE file_type{};DWRITE_FONT_FACE_TYPE face_type{};UINT32 face_count=0;
    if(SUCCEEDED(hr))hr=file->Analyze(&supported,&file_type,&face_type,&face_count);
    if(SUCCEEDED(hr) && (!supported || face_count!=1))hr=E_INVALIDARG;
    if(SUCCEEDED(hr))hr=factory->CreateFontFace(face_type,1,&file,0,DWRITE_FONT_SIMULATIONS_NONE,&face);
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
    if(selected_weight && SUCCEEDED(hr)){
        IDWriteFontFace5* original=nullptr;IDWriteFontResource* resource=nullptr;IDWriteFontFace5* selected=nullptr;
        hr=face->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&original));if(SUCCEEDED(hr))hr=original->GetFontResource(&resource);
        const DWRITE_FONT_AXIS_VALUE axis{DWRITE_FONT_AXIS_TAG_WEIGHT,static_cast<float>(selected_weight)};if(SUCCEEDED(hr))hr=resource->CreateFontFace(DWRITE_FONT_SIMULATIONS_NONE,&axis,1,&selected);
        if(SUCCEEDED(hr)){com_release(face);face=selected;selected=nullptr;}com_release(selected);com_release(resource);com_release(original);
    }
    if(measure_bmp){if(!bytes_equal(enabled,L"default",sizeof(L"default")) || rtl || sideways){com_release(face);com_release(file);com_release(analyzer);com_release(factory);return 2;}if(SUCCEEDED(hr))hr=bmp_complexity(analyzer,face);com_release(face);com_release(file);com_release(analyzer);com_release(factory);flush();return SUCCEEDED(hr) && output_ok?0:1;}
    if(measure_metrics){if(SUCCEEDED(hr))hr=measure_metric_cost?metric_throughput(face):metric_observation(face,selected_weight,!measure_font_metrics);com_release(face);com_release(file);com_release(analyzer);com_release(factory);flush();return SUCCEEDED(hr) && output_ok?0:1;}
    if(measure_ranges){if(SUCCEEDED(hr))hr=complexity_ranges(analyzer,face,analysis);com_release(face);com_release(file);com_release(analyzer);com_release(factory);flush();return SUCCEEDED(hr)&&output_ok?0:1;}
    if(measure_profile||measure_interior){if(SUCCEEDED(hr))hr=complexity_profile(analyzer,face,measure_interior);com_release(face);com_release(file);com_release(analyzer);com_release(factory);flush();return SUCCEEDED(hr)&&output_ok?0:1;}
#endif
    if(SUCCEEDED(hr))hr=analyzer->AnalyzeScript(&analysis,0,analysis.length,&analysis);
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
    wchar_t batch[2]{};
    if(GetEnvironmentVariableW(L"MAICORD_SHAPE_BATCH",batch,2)==1&&batch[0]=='1'){
        const bool ok=SUCCEEDED(hr)&&shape_batch_reference(analyzer,face,analysis.scripts[0],em_size,locale);
        com_release(face);com_release(file);com_release(analyzer);com_release(factory);return ok?0:1;
    }
#endif
#if defined(MAICORD_ITEMIZED_LAYOUT_REFERENCE)
    if(!measure_layout||sideways||!bytes_equal(enabled,L"default",sizeof(L"default")))hr=E_INVALIDARG;
    if(SUCCEEDED(hr))for(u32 i=0;i<analysis.length;++i)if(!analysis.seen[i])hr=E_UNEXPECTED;
    field(utf8_literal("{\"hresult\":"),static_cast<u32>(hr));field(utf8_literal(",\"em_bits\":"),em_bits);field(utf8_literal(",\"rtl\":"),rtl);
    print(utf8_literal(",\"script_items\":["));
    if(SUCCEEDED(hr))for(u32 first=0;first<analysis.length;){u32 end=first+1;const auto script=analysis.scripts[first];
        while(end<analysis.length&&analysis.scripts[end].script==script.script&&analysis.scripts[end].shapes==script.shapes)++end;
        if(first)print(utf8_literal(","));field(utf8_literal("["),first);field(utf8_literal(","),end-first);field(utf8_literal(","),script.script);field(utf8_literal(","),script.shapes);print(utf8_literal("]"));first=end;}
    print(utf8_literal("]"));if(!module_path())output_ok=false;
    if(SUCCEEDED(hr))hr=layout_observation(factory,face,analysis,em_size,nullptr,0,selected_weight,measure_hit_layout,measure_edit_layout);
    print(utf8_literal("}\n"));com_release(face);com_release(file);com_release(analyzer);com_release(factory);flush();return SUCCEEDED(hr)&&output_ok?0:1;
#else
    // This harness deliberately accepts one analyzed script run only.
    if(SUCCEEDED(hr))for(u32 i=0;i<analysis.length;++i)if(!analysis.seen[i] || analysis.scripts[i].script!=analysis.scripts[0].script || analysis.scripts[i].shapes!=analysis.scripts[0].shapes)hr=E_UNEXPECTED;
    UINT16 clusters[text_capacity]{},glyphs[glyph_capacity]{};
    DWRITE_SHAPING_TEXT_PROPERTIES text_props[text_capacity]{};DWRITE_SHAPING_GLYPH_PROPERTIES glyph_props[glyph_capacity]{};
    UINT32 actual=0;
    DWRITE_FONT_FEATURE feature_values[32]{};u32 feature_count=1;
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
    feature_count=0;
    if(!bytes_equal(enabled,L"default",sizeof(L"default"))){const wchar_t* p=enabled;
        while(*p){if(feature_count==32)return 2;u32 tag=0;for(u32 i=0;i<4;++i){if(*p<32 || *p>126 || *p=='=' || *p==',')return 2;tag|=static_cast<u32>(*p++)<<(i*8);}if(*p++!='=')return 2;
            u32 parameter=0,digits=0;while(*p && *p!=','){if(*p<'0' || *p>'9' || parameter>(0xffffffffu-static_cast<u32>(*p-'0'))/10)return 2;parameter=parameter*10+static_cast<u32>(*p++-'0');++digits;}if(!digits)return 2;
            feature_values[feature_count++]={static_cast<DWRITE_FONT_FEATURE_TAG>(tag),parameter};if(*p){++p;if(!*p)return 2;}}
    }
#else
    feature_values[0]={static_cast<DWRITE_FONT_FEATURE_TAG>(DWRITE_MAKE_OPENTYPE_TAG('t','e','s','t')),static_cast<UINT32>(enabled[0]-'0')};
#endif
    DWRITE_TYPOGRAPHIC_FEATURES features{feature_values,feature_count};const DWRITE_TYPOGRAPHIC_FEATURES* feature_sets[]={&features};
    const auto sets=feature_count?feature_sets:nullptr;const UINT32* lengths=feature_count?&analysis.length:nullptr;const u32 range_count=feature_count?1:0;
    if(SUCCEEDED(hr))hr=analyzer->GetGlyphs(analysis.text,analysis.length,face,sideways,rtl,&analysis.scripts[0],locale,nullptr,sets,lengths,range_count,glyph_capacity,clusters,text_props,glyphs,glyph_props,&actual);
    if(SUCCEEDED(hr) && actual>glyph_capacity)hr=E_UNEXPECTED;
    field(utf8_literal("{\"hresult\":"),static_cast<u32>(hr));field(utf8_literal(",\"script\":"),analysis.scripts[0].script);field(utf8_literal(",\"shapes\":"),analysis.scripts[0].shapes);
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
    field(utf8_literal(",\"feature_count\":"),feature_count);
#else
    field(utf8_literal(",\"feature_enabled\":"),feature_values[0].parameter);
#endif
#ifdef MAICORD_PLACEMENT_REFERENCE
    float advances[glyph_capacity]{};DWRITE_GLYPH_OFFSET offsets[glyph_capacity]{};
    const HRESULT placement_hr=SUCCEEDED(hr)?analyzer->GetGlyphPlacements(analysis.text,clusters,text_props,analysis.length,glyphs,glyph_props,actual,face,em_size,sideways,rtl,&analysis.scripts[0],locale,sets,lengths,range_count,advances,offsets):hr;
    field(utf8_literal(",\"placement_hresult\":"),static_cast<u32>(placement_hr));field(utf8_literal(",\"em_bits\":"),em_bits);field(utf8_literal(",\"rtl\":"),rtl);field(utf8_literal(",\"sideways\":"),sideways);
    if(SUCCEEDED(placement_hr)){
        print(utf8_literal(",\"placement_bits\":["));for(u32 i=0;i<actual;++i){u32 a=0,x=0,y=0;copy_bytes(&a,&advances[i],4);copy_bytes(&x,&offsets[i].advanceOffset,4);copy_bytes(&y,&offsets[i].ascenderOffset,4);if(i)print(utf8_literal(","));field(utf8_literal("["),a);field(utf8_literal(","),x);field(utf8_literal(","),y);print(utf8_literal("]"));}print(utf8_literal("]"));
    }else output_ok=false;
#endif
    if(!module_path())output_ok=false;
    if(SUCCEEDED(hr)){
        print(utf8_literal(",\"glyphs\":["));for(u32 i=0;i<actual;++i){if(i)print(utf8_literal(","));number(glyphs[i]);}
        print(utf8_literal("],\"clusters\":["));for(u32 i=0;i<analysis.length;++i){if(i)print(utf8_literal(","));number(clusters[i]);}
        print(utf8_literal("],\"text_shaped_alone\":["));for(u32 i=0;i<analysis.length;++i){if(i)print(utf8_literal(","));number(text_props[i].isShapedAlone);}
        print(utf8_literal("],\"glyph_properties\":["));for(u32 i=0;i<actual;++i){if(i)print(utf8_literal(","));
            field(utf8_literal("["),glyph_props[i].justification);field(utf8_literal(","),glyph_props[i].isClusterStart);field(utf8_literal(","),glyph_props[i].isDiacritic);field(utf8_literal(","),glyph_props[i].isZeroWidthSpace);print(utf8_literal("]"));}
        print(utf8_literal("]"));
    }
#ifdef MAICORD_FEATURE_PLAN_REFERENCE
    if(measure_layout && SUCCEEDED(hr)){
        IDWriteTextAnalyzer1* a1=nullptr;BOOL simple=FALSE;UINT32 read=0;UINT16 nominal[text_capacity]{};
        HRESULT complexity_hr=analyzer->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&a1));
        if(SUCCEEDED(complexity_hr))complexity_hr=a1->GetTextComplexity(analysis.text,analysis.length,face,&simple,&read,nominal);
        field(utf8_literal(",\"complexity_hresult\":"),static_cast<u32>(complexity_hr));field(utf8_literal(",\"text_simple\":"),simple);field(utf8_literal(",\"complexity_length\":"),read);
        com_release(a1);if(FAILED(complexity_hr) || FAILED(layout_observation(factory,face,analysis,em_size,feature_values,feature_count,selected_weight,measure_hit_layout,measure_edit_layout)))output_ok=false;
    }
#endif
    print(utf8_literal("}\n"));com_release(face);com_release(file);com_release(analyzer);com_release(factory);flush();return SUCCEEDED(hr) && output_ok?0:1;
#endif
}
} }
extern "C" __declspec(noreturn) void WINAPI maicord_gsub_execution_reference_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
