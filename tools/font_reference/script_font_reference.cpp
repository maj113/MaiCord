// QA only: batch font-dependent script/language choices with marker fonts.
#define maicord_script_analysis_reference_entry maicord_unused_script_analysis_entry
#include "script_analysis_reference.cpp"
#undef maicord_script_analysis_reference_entry
namespace maicord {namespace {
bool font_argument(wchar_t* output,u32 capacity) noexcept {
    const wchar_t* p=GetCommandLineW();bool quoted=false;
    while(*p){if(*p=='"')quoted=!quoted;else if(!quoted&&(*p==' '||*p=='\t'))break;++p;}
    while(*p==' '||*p=='\t')++p;quoted=*p=='"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p=='"':(*p==' '||*p=='\t'))){if(n+1>=capacity)return false;output[n++]=*p++;}
    if(quoted){if(*p!='"')return false;++p;}while(*p==' '||*p=='\t')++p;output[n]=0;return n&&!*p;
}
bool font_observe(IDWriteTextAnalyzer1* analyzer,IDWriteFontFace* face) noexcept {
    const auto read=[](void* p,u32 n) noexcept {DWORD got=0;return ReadFile(GetStdHandle(STD_INPUT_HANDLE),p,n,&got,nullptr)&&got==n;};
    u32 header[2]{};if(!read(header,8)||(header[0]!=0x31504653&&header[0]!=0x32504653)||!header[1]||header[1]>100000||!save(header,8))return false;
    Analysis<128> input;u32 scalars[64];u16 clusters[128],glyphs[1024],analysis[128];
    DWRITE_SHAPING_TEXT_PROPERTIES text_properties[128];DWRITE_SHAPING_GLYPH_PROPERTIES glyph_properties[1024];
    float advances[1024];DWRITE_GLYPH_OFFSET offsets[1024];
    DWRITE_FONT_FEATURE feature_values[32];
    for(u32 row=0;row<header[1];++row){u32 count=0,rtl=0;
        if(!read(&count,4)||!count||count>64||!read(&rtl,4)||rtl>1)return false;u32 feature_count=0;
        if(header[0]==0x32504653&&(!read(&feature_count,4)||feature_count>32||!read(feature_values,feature_count*sizeof(DWRITE_FONT_FEATURE))))return false;
        if(!read(scalars,count*4))return false;input.length=0;
        for(u32 i=0;i<count;++i){const u32 cp=scalars[i];if(cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
            if(cp>65535){input.text[input.length++]=static_cast<wchar_t>(0xd800+((cp-0x10000)>>10));input.text[input.length++]=static_cast<wchar_t>(0xdc00+((cp-0x10000)&1023));}
            else input.text[input.length++]=static_cast<wchar_t>(cp);}
        for(u32 i=0;i<input.length;++i)input.scripts[i]={0x7fff,DWRITE_SCRIPT_SHAPES_DEFAULT};
        if(FAILED(analyzer->AnalyzeScript(&input,0,input.length,&input)))return false;
        for(u32 i=0;i<input.length;++i){if(!packed(analyzer,input.scripts[i],analysis+i)||input.scripts[i].script!=input.scripts[0].script||input.scripts[i].shapes!=input.scripts[0].shapes)return false;}
        DWRITE_TYPOGRAPHIC_FEATURES features{feature_values,feature_count};const DWRITE_TYPOGRAPHIC_FEATURES* sets[]{&features};const u32 ranges=feature_count?1u:0u;
        u32 glyph_count=0;const auto status=analyzer->GetGlyphs(input.text,input.length,face,FALSE,rtl!=0,input.scripts,L"en-US",nullptr,feature_count?sets:nullptr,feature_count?&input.length:nullptr,ranges,1024,clusters,text_properties,glyphs,glyph_properties,&glyph_count);
        HRESULT placement=E_FAIL;
        if(SUCCEEDED(status)){if(glyph_count>1024)return false;
            placement=analyzer->GetGlyphPlacements(input.text,clusters,text_properties,input.length,glyphs,glyph_properties,glyph_count,face,1000,FALSE,rtl!=0,input.scripts,L"en-US",feature_count?sets:nullptr,feature_count?&input.length:nullptr,ranges,advances,offsets);}
        const u32 output_header[]{input.length,SUCCEEDED(status)?glyph_count:0,static_cast<u32>(status),static_cast<u32>(placement)};
        if(!save(output_header,sizeof(output_header))||!save(analysis,input.length*2))return false;
        if(SUCCEEDED(status)&&(!save(clusters,input.length*2)||!save(text_properties,input.length*sizeof(DWRITE_SHAPING_TEXT_PROPERTIES))||!save(glyphs,glyph_count*2)||!save(glyph_properties,glyph_count*sizeof(DWRITE_SHAPING_GLYPH_PROPERTIES))))return false;
        if(SUCCEEDED(placement)&&(!save(advances,glyph_count*4)||!save(offsets,glyph_count*sizeof(DWRITE_GLYPH_OFFSET))))return false;
    }
    u8 tail=0;DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),&tail,1,&got,nullptr)||got)return false;
    return save(&script_end,4)&&save(properties,script_end*sizeof(DWRITE_SCRIPT_PROPERTIES))&&flush();
}
int font_run() noexcept {
    wchar_t path[32768];if(!font_argument(path,32768))return 2;
    IDWriteFactory* factory=nullptr;IDWriteTextAnalyzer* base=nullptr;IDWriteTextAnalyzer1* analyzer=nullptr;IDWriteFontFile* file=nullptr;IDWriteFontFace* face=nullptr;
    auto hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateTextAnalyzer(&base);if(SUCCEEDED(hr))hr=base->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&analyzer));
    if(SUCCEEDED(hr))hr=factory->CreateFontFileReference(path,nullptr,&file);BOOL supported=FALSE;DWRITE_FONT_FILE_TYPE file_type{};DWRITE_FONT_FACE_TYPE face_type{};u32 face_count=0;
    if(SUCCEEDED(hr))hr=file->Analyze(&supported,&file_type,&face_type,&face_count);
    if(SUCCEEDED(hr)&&supported&&face_count==1)hr=factory->CreateFontFace(face_type,1,&file,0,DWRITE_FONT_SIMULATIONS_NONE,&face);else hr=E_FAIL;
    const bool okay=SUCCEEDED(hr)&&font_observe(analyzer,face);
    if(face)face->Release();if(file)file->Release();if(analyzer)analyzer->Release();if(base)base->Release();if(factory)factory->Release();return okay?0:1;
}
}}
extern "C" int maicord_script_font_reference_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::font_run()));}
