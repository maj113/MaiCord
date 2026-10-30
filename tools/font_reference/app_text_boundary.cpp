// QA only: observe MaiCord's actual profile/style/emoji/fallback text path.
// The renderer owner is inert; this tool creates no window or graphics device.
#include "maicord/render/d3d11_renderer.hpp"
#include "native_run_reference.hpp"
#include "maicord/core/byte_order.hpp"
#include <dwrite_3.h>
#include "maicord/ui/native_text_candidate.hpp"

namespace maicord { namespace {
constexpr u32 capacity=4096,face_capacity=512;
char output_bytes[65536];u32 output_count=0;bool output_ok=true;
void flush() noexcept {
    DWORD written=0;
    if(output_count)output_ok=output_ok&&WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),output_bytes,output_count,&written,nullptr)&&written==output_count;
    output_count=0;
}
void ch(char value) noexcept {if(output_count==sizeof(output_bytes))flush();output_bytes[output_count++]=value;}
void text(const char* value) noexcept {while(*value)ch(*value++);}
void number(u32 value) noexcept {char digits[16];u32 count=0;do{digits[count++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(count)ch(digits[--count]);}
void bits(float value) noexcept {u32 result=0;copy_bytes(&result,&value,4);number(result);}
void field(const char* name,u32 value) noexcept {text(name);number(value);}
void wide(const wchar_t* value) noexcept {
    constexpr char hex[]="0123456789abcdef";ch('"');
    while(*value){const u32 cp=*value++;text("\\u");for(int shift=12;shift>=0;shift-=4)ch(hex[(cp>>shift)&15]);}ch('"');
}
bool argument(const wchar_t** command,wchar_t* output,u32 available) noexcept {
    const wchar_t* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 count=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){if(count+1>=available)return false;output[count++]=*p++;}
    if(quoted){if(*p!='"')return false;++p;}output[count]=0;*command=p;return count!=0;
}
struct Reader {
    const u8* data;u32 size,at;
    bool word(u32* value) noexcept {if(at>size||size-at<4)return false;*value=byte_load_le32(data+at);at+=4;return true;}
    bool take(u32 count,Utf8View* value) noexcept {if(at>size||count>size-at)return false;*value={reinterpret_cast<const char*>(data+at),count};at+=count;return true;}
};
struct Analysis final : IDWriteTextAnalysisSource,IDWriteTextAnalysisSink {
    wchar_t units[capacity]{};DWRITE_SCRIPT_ANALYSIS scripts[capacity]{};bool seen[capacity]{};u32 count=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID))||bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSource),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSource*>(this);
        else if(bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSink),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSink*>(this);
        else return E_NOINTERFACE;return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 at,WCHAR const** value,UINT32* length) override {*value=at<count?units+at:nullptr;*length=at<count?count-at:0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 at,WCHAR const** value,UINT32* length) override {if(at>count)return E_INVALIDARG;*value=at?units:nullptr;*length=at;return S_OK;}
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;}
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 at,UINT32* length,WCHAR const** value) override {if(at>count)return E_INVALIDARG;*length=count-at;*value=L"en-US";return S_OK;}
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 at,UINT32* length,IDWriteNumberSubstitution** value) override {if(at>count)return E_INVALIDARG;*length=count-at;*value=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32 at,UINT32 length,DWRITE_SCRIPT_ANALYSIS const* value) override {
        if(!value||at>count||length>count-at)return E_INVALIDARG;
        for(u32 i=at;i<at+length;++i){if(seen[i])return E_UNEXPECTED;scripts[i]=*value;seen[i]=true;}return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32,UINT32,DWRITE_LINE_BREAKPOINT const*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32,UINT32,IDWriteNumberSubstitution*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32,UINT32,UINT8,UINT8) override {return E_NOTIMPL;}
    bool decode(Utf8View input) noexcept {
        count=0;for(u32 at=0;at<input.bytes;){const auto cp=utf8_decode_one(input.data+at,input.bytes-at);if(!cp.valid)return false;at+=cp.width;
            if(capacity-count<(cp.codepoint>0xffff?2u:1u))return false;
            if(cp.codepoint<=0xffff)units[count++]=static_cast<wchar_t>(cp.codepoint);
            else{const u32 v=cp.codepoint-0x10000;units[count++]=static_cast<wchar_t>(0xd800+(v>>10));units[count++]=static_cast<wchar_t>(0xdc00+(v&1023));}}
        for(u32 i=0;i<count;++i)seen[i]=false;return true;
    }
};
D3D11Renderer renderer{};DWriteTextSystem oracle{};Analysis analysis{};IDWriteTextAnalyzer1* analyzer=nullptr;
NativeFontSession* native_fonts=nullptr;Arena native_scratch{};
IDWriteFontFace5* faces[face_capacity]{};u32 face_count=0,case_count=0,app_failures=0;
TextGlyph glyphs[capacity*4];TextRun runs[capacity];u16 clusters[capacity];TextDecoration decorations[capacity];
ShapedText shaped{};u32 run_faces[capacity];wchar_t utf16[capacity];u32 failure_stage=0,failure_error=0;
struct RawCapture final : IDWriteTextRenderer {
    u32 id;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID))&&!bytes_equal(&iid,&__uuidof(IDWriteTextRenderer),sizeof(GUID))&&!bytes_equal(&iid,&__uuidof(IDWritePixelSnapping),sizeof(GUID)))return E_NOINTERFACE;
        *out=static_cast<IDWriteTextRenderer*>(this);return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* value) override {*value=TRUE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* value) override {*value={1,0,0,1,0,0};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value) override {*value=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT,FLOAT,DWRITE_MEASURING_MODE,const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION* description,IUnknown*) override {
        field("{\"kind\":\"raw_run\",\"id\":",id);field(",\"glyph_count\":",run->glyphCount);
        field(",\"position\":",description?description->textPosition:0);field(",\"length\":",description?description->stringLength:0);text(",\"clusters\":[");
        if(description)for(u32 i=0;i<description->stringLength;++i){if(i)ch(',');number(description->clusterMap[i]);}text("]}\n");return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override {return E_NOTIMPL;}
};
void status(NativeFontResult result) noexcept {
    field("{\"error\":",static_cast<u32>(result.error));field(",\"font_status\":",static_cast<u32>(result.font_status));field(",\"native_error\":",result.native_error);ch('}');
}
NativeFontResult bind(IDWriteFontFace* selected,bool instructions,u32* geometry_kind=nullptr) noexcept {
    NativeFontInstanceHandle handle{};
    bool oblique=false,bold=false;
    const auto result=native_font_bind_selected(native_fonts,selected,instructions,&native_scratch,&handle,instructions?&oblique:nullptr,instructions?&bold:nullptr);
    if(result.ok()){
        const auto* instance=native_font_instance_get(*native_font_session_instances(native_fonts),handle);
        if(geometry_kind)*geometry_kind=static_cast<u32>(instance->geometry.font->kind);
        if(!native_font_instance_release(native_font_session_instances(native_fonts),handle))output_ok=false;
    }
    return result;
}
bool face_id(IDWriteFontFace* selected,u32* id) noexcept {
    for(u32 i=0;i<face_count;++i)if(faces[i]->Equals(selected)){*id=i;return true;}
    if(face_count==face_capacity)return false;
    IDWriteFontFace5* face=nullptr;HRESULT hr=selected->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&face));if(FAILED(hr))return false;
    *id=face_count;faces[face_count++]=face;
    IDWriteFontFile* file=nullptr;IDWriteFontFileLoader* loader=nullptr;IDWriteLocalFontFileLoader* local=nullptr;IDWriteLocalizedStrings* names=nullptr;
    struct Release {IDWriteFontFile*& f;IDWriteFontFileLoader*& l;IDWriteLocalFontFileLoader*& p;IDWriteLocalizedStrings*& n;~Release() noexcept {com_release(n);com_release(p);com_release(l);com_release(f);}} release{file,loader,local,names};
    u32 files=0;hr=face->GetFiles(&files,nullptr);if(FAILED(hr)||files!=1)return false;
    hr=face->GetFiles(&files,&file);const void* key=nullptr;u32 key_size=0,path_count=0;
    if(SUCCEEDED(hr))hr=file->GetReferenceKey(&key,&key_size);
    if(SUCCEEDED(hr))hr=file->GetLoader(&loader);
    if(SUCCEEDED(hr))hr=loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),reinterpret_cast<void**>(&local));
    if(SUCCEEDED(hr))hr=local->GetFilePathLengthFromKey(key,key_size,&path_count);
    wchar_t path[32768];if(FAILED(hr)||path_count>=32768)return false;
    hr=local->GetFilePathFromKey(key,key_size,path,32768);if(FAILED(hr))return false;
    hr=face->GetFamilyNames(&names);wchar_t family[256];if(SUCCEEDED(hr))hr=names->GetString(0,family,256);if(FAILED(hr))return false;
    DWRITE_FONT_AXIS_VALUE axes[64];const u32 axis_count=face->GetFontAxisValueCount();if(axis_count>64)return false;
    hr=face->GetFontAxisValues(axes,axis_count);if(FAILED(hr))return false;
    field("{\"kind\":\"face\",\"id\":",*id);text(",\"path\":");wide(path);text(",\"family\":");wide(family);
    field(",\"index\":",face->GetIndex());field(",\"simulations\":",face->GetSimulations());field(",\"type\":",face->GetType());field(",\"glyph_count\":",face->GetGlyphCount());
    text(",\"axes\":[");for(u32 i=0;i<axis_count;++i){if(i)ch(',');ch('[');number(axes[i].axisTag);ch(',');bits(axes[i].value);ch(']');}ch(']');
    text(",\"tables\":[");constexpr u32 tags[]{DWRITE_MAKE_OPENTYPE_TAG('C','O','L','R'),DWRITE_MAKE_OPENTYPE_TAG('C','P','A','L'),DWRITE_MAKE_OPENTYPE_TAG('C','B','D','T'),DWRITE_MAKE_OPENTYPE_TAG('s','b','i','x'),DWRITE_MAKE_OPENTYPE_TAG('S','V','G',' '),DWRITE_MAKE_OPENTYPE_TAG('C','F','F','2')};
    for(u32 i=0;i<6;++i){const void* table=nullptr;void* context=nullptr;u32 size=0;BOOL exists=FALSE;hr=face->TryGetFontTable(tags[i],&table,&size,&context,&exists);if(context)face->ReleaseFontTable(context);if(FAILED(hr))return false;if(i)ch(',');number(exists?size:0);}ch(']');
    u32 kind=~0u;const auto raw=bind(face,false,&kind),instructed=bind(face,true);
    text(",\"native_raw\":");status(raw);text(",\"native_outline\":");status(instructed);field(",\"native_geometry_kind\":",kind);text("}\n");return true;
}
bool capture(u32 id,u32 profile,u32 style,bool independent,float width,float height,Utf8View input) noexcept {
    failure_stage=1;failure_error=0;
    if(profile>=static_cast<u32>(UiTextFace::count)||!analysis.decode(input))return false;
    failure_stage=2;HRESULT hr=analyzer->AnalyzeScript(&analysis,0,analysis.count,&analysis);if(FAILED(hr)){failure_error=static_cast<u32>(hr);return false;}
    failure_stage=3;
    for(u32 i=0;i<analysis.count;++i)if(!analysis.seen[i])return false;
    auto system=renderer_text_system(&renderer,static_cast<UiTextFace>(profile));system.independent_letters=independent;
    // A missing private font must fail the observation instead of silently
    // reporting a system replacement as the intended application font.
    failure_stage=4;if(profile>=static_cast<u32>(UiTextFace::techno)&&!renderer.native_text_faces[profile])return false;
    shaped_text_reset(&shaped);const TextStyleSpan span{0,input.bytes,style};
    const auto before=native_text_candidate_counts(system);
    const auto result=text_shape_utf8(&system,input,style?ConstSlice<TextStyleSpan>{&span,1}:ConstSlice<TextStyleSpan>{},width,height,{utf16,capacity},&shaped);
    const auto after=native_text_candidate_counts(system);
    if(after.arabic_shaped>before.arabic_shaped){
        const auto measured=text_measure_utf8(&system,input,{},width,height,{utf16,capacity});
        const auto measured_counts=native_text_candidate_counts(system);
        if(!measured.ok() || measured_counts.arabic_measured!=after.arabic_measured+1 ||
           !bytes_equal(&measured.width,&shaped.width,4) || !bytes_equal(&measured.width_including_trailing_whitespace,&shaped.width_including_trailing_whitespace,4) ||
           !bytes_equal(&measured.height,&shaped.height,4) || measured.utf16_units!=result.utf16_units || measured.line_count!=shaped.line_count)return false;
        field("{\"kind\":\"native_arabic_dispatch\",\"id\":",id);text(",\"shaped\":1,\"measured\":1}\n");
    }
    failure_stage=5;if(result.utf16_units!=analysis.count)return false;
    if(!result.ok()){
        if(!profile&&!style&&!independent){
            IDWriteTextLayout* layout=nullptr;auto raw_hr=oracle.factory->CreateTextLayout(analysis.units,analysis.count,oracle.format,width,height,&layout);
            if(SUCCEEDED(raw_hr))raw_hr=text_apply_layout_styles(oracle,layout,input,{});
            RawCapture raw{};raw.id=id;if(SUCCEEDED(raw_hr))raw_hr=layout->Draw(nullptr,&raw,0,0);com_release(layout);
            field("{\"kind\":\"raw_status\",\"id\":",id);field(",\"hresult\":",static_cast<u32>(raw_hr));text("}\n");
        }
        field("{\"kind\":\"case\",\"id\":",id);field(",\"profile\":",profile);field(",\"style\":",style);field(",\"independent\":",independent?1:0);
        field(",\"error\":",static_cast<u32>(result.error));field(",\"native_error\":",static_cast<u32>(result.native_error));field(",\"utf16_units\":",result.utf16_units);text("}\n");
        ++case_count;++app_failures;return true;
    }
    failure_stage=6;
    for(u32 i=0;i<shaped.run_count;++i){IDWriteFontFace* selected=nullptr;
        if(!native_run_reference(oracle.factory,shaped,shaped.runs[i],&selected))return false;
        const bool captured=face_id(selected,&run_faces[i]);com_release(selected);if(!captured)return false;}
    field("{\"kind\":\"case\",\"id\":",id);field(",\"profile\":",profile);field(",\"style\":",style);field(",\"independent\":",independent?1:0);
    field(",\"utf16_units\":",result.utf16_units);text(",\"tracking_bits\":");bits(system.tracking);field(",\"fixed_weight\":",system.fixed_weight?1:0);
    text(",\"metrics_bits\":[");bits(shaped.width);ch(',');bits(shaped.width_including_trailing_whitespace);ch(',');bits(shaped.height);field("],\"line_count\":",shaped.line_count);
    text(",\"scripts\":[");bool comma=false;for(u32 first=0;first<analysis.count;){u32 end=first+1;const auto script=analysis.scripts[first];
        while(end<analysis.count&&analysis.scripts[end].script==script.script&&analysis.scripts[end].shapes==script.shapes)++end;
        DWRITE_SCRIPT_PROPERTIES properties{};hr=analyzer->GetScriptProperties(script,&properties);if(FAILED(hr))return false;
        if(comma)ch(',');comma=true;ch('[');number(first);ch(',');number(end-first);ch(',');number(script.script);ch(',');number(properties.isoScriptCode);ch(',');number(script.shapes);ch(']');first=end;}ch(']');
    text(",\"runs\":[");for(u32 i=0;i<shaped.run_count;++i){const auto& run=shaped.runs[i];if(i)ch(',');ch('[');number(run_faces[i]);ch(',');number(run.text_position);ch(',');number(run.text_length);ch(',');number(run.first_glyph);ch(',');number(run.glyph_count);ch(',');number(run.bidi_level);ch(',');bits(run.baseline_x);ch(',');bits(run.baseline_y);ch(',');bits(run.em_size);ch(',');number(run.style_flags);ch(']');}ch(']');
    text(",\"glyphs\":[");for(u32 i=0;i<shaped.glyph_count;++i){const auto& glyph=shaped.glyphs[i];if(i)ch(',');ch('[');number(glyph.index);ch(',');bits(glyph.advance);ch(',');bits(glyph.advance_offset);ch(',');bits(glyph.ascender_offset);ch(']');}ch(']');
    text(",\"clusters\":[");for(u32 i=0;i<shaped.cluster_count;++i){if(i)ch(',');number(shaped.clusters[i]);}text("]}\n");++case_count;return true;
}
bool run() noexcept {
    const wchar_t* command=GetCommandLineW();wchar_t path[32768];if(!argument(&command,path,32768)||!argument(&command,path,32768))return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};bool ok=GetFileSizeEx(file,&size)&&size.QuadPart>=12&&size.QuadPart<=32*1024*1024;
    u8* bytes=ok?static_cast<u8*>(VirtualAlloc(nullptr,static_cast<usize>(size.QuadPart),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)):nullptr;DWORD received=0;
    ok=bytes&&ReadFile(file,bytes,static_cast<DWORD>(size.QuadPart),&received,nullptr)&&received==size.QuadPart;CloseHandle(file);
    if(ok)ok=bytes_equal(bytes,"MCTEXTB1",8);
    IDWriteTextAnalyzer* base=nullptr;
    if(ok)ok=text_system_initialize(&renderer.glyph_text.system,L"Segoe UI",16).ok();
    if(ok)ok=text_system_initialize(&oracle,L"Segoe UI",16).ok();
    if(ok)ok=SUCCEEDED(oracle.factory->CreateTextAnalyzer(&base))&&SUCCEEDED(base->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&analyzer)));
    com_release(base);
    if(ok)ok=shaped_text_initialize(&shaped,{glyphs,capacity*4},{runs,capacity},{clusters,capacity},{decorations,capacity})&&native_font_session_create(&native_fonts)&&arena_initialize(&native_scratch,4*1024*1024,0,4096);
    Reader reader{bytes,static_cast<u32>(size.QuadPart),8};u32 count=0;if(ok)ok=reader.word(&count)&&count&&count<=100000;
    for(u32 i=0;i<count&&ok;++i){u32 id=0,profile=0,style=0,independent=0,wb=0,hb=0,length=0;Utf8View input{};float width=0,height=0;
        ok=reader.word(&id)&&reader.word(&profile)&&reader.word(&style)&&reader.word(&independent)&&reader.word(&wb)&&reader.word(&hb)&&reader.word(&length)&&reader.take(length,&input)&&length&&length<=capacity&&independent<=1;
        copy_bytes(&width,&wb,4);copy_bytes(&height,&hb,4);if(ok)ok=capture(id,profile,style,independent!=0,width,height,input);
        if(!ok){field("{\"kind\":\"failure\",\"id\":",id);field(",\"stage\":",failure_stage);field(",\"error\":",failure_error);text("}\n");}}
    if(ok)ok=reader.at==reader.size;
    shaped_text_reset(&shaped);for(u32 i=0;i<face_count;++i)com_release(faces[i]);
    text_system_release(&renderer.glyph_text.system);text_system_release(&oracle);com_release(analyzer);arena_release(&native_scratch);if(native_fonts)ok=native_font_session_release(&native_fonts)&&ok;
    if(bytes)VirtualFree(bytes,0,MEM_RELEASE);field("{\"kind\":\"summary\",\"cases\":",case_count);field(",\"faces\":",face_count);field(",\"app_failures\":",app_failures);field(",\"ok\":",ok?1:0);text("}\n");flush();return ok&&output_ok;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_app_text_boundary_entry() noexcept {ExitProcess(maicord::run()?0:1);}
