// QA-only Windows observations. Every official vector and the given probes are compared
// with line_breaks_tailored at interior scalar boundaries; raw UTF-16 before/after
// conditions and endpoints are retained without inventing neutral policy.
#include "maicord/text/line_break.hpp"
#include "maicord/text/detail/line_break_tailored.hpp"
#include "maicord/core/byte_order.hpp"
#include <dwrite.h>
namespace maicord {namespace {
#include "../../tests/generated/line_break_test.inc"
constexpr u32 capacity=256;
char output[65536];u32 output_size=0;bool output_ok=true;
void flush() noexcept {DWORD done=0;if(output_size&&(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),output,output_size,&done,nullptr)||done!=output_size))output_ok=false;output_size=0;}
void print(Utf8View text) noexcept {for(u32 i=0;i<text.bytes;++i){if(output_size==sizeof(output))flush();output[output_size++]=text.data[i];}}
void number(u32 value) noexcept {char s[16];u32 n=0;do{s[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){char t=s[i];s[i]=s[n-i-1];s[n-i-1]=t;}print({s,n});}
void field(u32 n) noexcept {print(utf8_literal(","));number(n);}
void vector(ConstSlice<u8> v) noexcept {print(utf8_literal(","));for(u32 i=0;i<v.count;++i){if(i)print(utf8_literal(" "));number(v[i]);}}
struct Analysis final : IDWriteTextAnalysisSource,IDWriteTextAnalysisSink {
    wchar_t text[capacity*2];DWRITE_LINE_BREAKPOINT breaks[capacity*2];u8 seen[capacity*2];u32 length=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID))||bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSource),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSource*>(this);
        else if(bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSink),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSink*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 at,WCHAR const** value,UINT32* count) override {*value=at<length?text+at:nullptr;*count=at<length?length-at:0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 at,WCHAR const** value,UINT32* count) override {if(at>length)return E_INVALIDARG;*value=at?text:nullptr;*count=at;return S_OK;}
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;}
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 at,UINT32* count,WCHAR const** value) override {if(at>length)return E_INVALIDARG;*count=length-at;*value=L"en-US";return S_OK;}
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 at,UINT32* count,IDWriteNumberSubstitution** value) override {if(at>length)return E_INVALIDARG;*count=length-at;*value=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32 at,UINT32 count,DWRITE_LINE_BREAKPOINT const* values) override {
        if(at>length||count>length-at||(count&&!values))return E_INVALIDARG;
        for(u32 i=0;i<count;++i){if(seen[at+i])return E_FAIL;breaks[at+i]=values[i];seen[at+i]=1;}return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32,UINT32,DWRITE_SCRIPT_ANALYSIS const*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32,UINT32,UINT8,UINT8) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32,UINT32,IDWriteNumberSubstitution*) override {return E_NOTIMPL;}
};
// Priority MUST > MAY_NOT > CAN > NEUTRAL is specified by DWRITE_BREAK_CONDITION.
u8 boundary(const Analysis& a,u32 at) noexcept {
    const u8 left=at?a.breaks[at-1].breakConditionAfter:0,right=at<a.length?a.breaks[at].breakConditionBefore:0;
    const u8 value=maximum(left,right);return value==3?2:value==2?0:value==1?1:255;
}
struct Probes {
    u8* bytes=nullptr;u32 size=0,at=0,count=0;bool active=false;
    ~Probes() noexcept {if(bytes)VirtualFree(bytes,0,MEM_RELEASE);}
    bool word(u32* value) noexcept {if(size-at<4)return false;*value=byte_load_le32(bytes+at);at+=4;return true;}
    bool load() noexcept {
        const wchar_t* c=GetCommandLineW();bool quote=false;
        while(*c){if(*c=='"')quote=!quote;else if(!quote&&(*c==' '||*c=='\t'))break;++c;}while(*c==' '||*c=='\t')++c;
        if(!*c)return true;active=true;wchar_t path[32768];u32 n=0;quote=*c=='"';if(quote)++c;
        while(*c&&!(quote?*c=='"':*c==' '||*c=='\t')){if(n+1>=32768)return false;path[n++]=*c++;}path[n]=0;
        if(quote){if(*c!='"')return false;++c;}while(*c==' '||*c=='\t')++c;if(*c||!n)return false;
        HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
        struct Close {HANDLE file;~Close() noexcept {CloseHandle(file);}} close{file};LARGE_INTEGER length{};
        if(!GetFileSizeEx(file,&length)||length.QuadPart<12||length.QuadPart>32*1024*1024)return false;size=static_cast<u32>(length.QuadPart);
        bytes=static_cast<u8*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!bytes)return false;DWORD done=0;
        if(!ReadFile(file,bytes,size,&done,nullptr)||done!=size||!bytes_equal(bytes,"MCLBPRB1",8))return false;at=8;return word(&count)&&count>0;
    }
};
bool run() noexcept {
    Probes probes;if(!probes.load())return false;
    IDWriteFactory* factory=nullptr;IDWriteTextAnalyzer* analyzer=nullptr;
    struct Release {IDWriteFactory*& f;IDWriteTextAnalyzer*& a;~Release() noexcept {if(a)a->Release();if(f)f->Release();}} release{factory,analyzer};
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory)))||FAILED(factory->CreateTextAnalyzer(&analyzer)))return false;
    Analysis a;u32 text[capacity],starts[capacity+1];u8 candidate[capacity+1],windows[capacity+1],before[capacity*2],after[capacity*2],white[capacity*2],hyphen[capacity*2];u32 at=0,cases=0;
    constexpr u32 size=sizeof(line_vectors)/sizeof(line_vectors[0]);
    print(utf8_literal("source_line,scalars,utf16_units,checked,unresolved,surrogate_breaks,windows,before,after,whitespace,soft_hyphen,candidate_differences,candidate,has_expected\n"));
    while(probes.active?cases<probes.count:at<size){
        u32 line=0,n=0;
        if(probes.active){if(!probes.word(&line)||!probes.word(&n)||line!=cases+1)return false;}
        else {if(size-at<2)return false;n=line_vectors[at++]&0x7fffffff;line=line_vectors[at++];if(n>size-at)return false;}
        if(!n||n>capacity)return false;a.length=0;
        for(u32 i=0;i<n;++i){u32 cp=0;if(probes.active){if(!probes.word(&cp))return false;}else cp=line_vectors[at+i]&0x1fffff;text[i]=cp;starts[i]=a.length;
            if(cp<=0xffff)a.text[a.length++]=static_cast<wchar_t>(cp);
            else{cp-=0x10000;a.text[a.length++]=static_cast<wchar_t>(0xd800+(cp>>10));a.text[a.length++]=static_cast<wchar_t>(0xdc00+(cp&1023));}}
        starts[n]=a.length;
        if(!line_breaks_tailored({text,n},{candidate,n+1}))return false;
        for(u32 i=0;i<a.length;++i)a.seen[i]=0;
        if(FAILED(analyzer->AnalyzeLineBreakpoints(&a,0,a.length,&a)))return false;
        for(u32 i=0;i<a.length;++i){if(!a.seen[i])return false;const auto b=a.breaks[i];before[i]=b.breakConditionBefore;after[i]=b.breakConditionAfter;white[i]=b.isWhitespace;hyphen[i]=b.isSoftHyphen;}
        u32 checked=0,unresolved=0,surrogates=0;
        for(u32 i=0;i<=n;++i){windows[i]=boundary(a,starts[i]);
            if(i&&i<n){if(windows[i]==255)++unresolved;else ++checked;}
            if(i<n&&starts[i+1]-starts[i]==2){const u8 inner=boundary(a,starts[i]+1);if(inner==1||inner==2)++surrogates;}}
        number(line);field(n);field(a.length);field(checked);field(unresolved);field(surrogates);
        vector({windows,n+1});vector({before,a.length});vector({after,a.length});vector({white,a.length});vector({hyphen,a.length});
        u32 candidate_differences=0;for(u32 i=1;i<n;++i)if(windows[i]!=255&&candidate[i]!=windows[i])++candidate_differences;
        field(candidate_differences);vector({candidate,n+1});field(probes.active?0:1);print(utf8_literal("\n"));
        if(!probes.active)at+=n;++cases;
    }
    return probes.active?probes.at==probes.size:cases==line_vector_count;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_line_reference_entry() noexcept {const bool ok=maicord::run();maicord::flush();ExitProcess(ok&&maicord::output_ok?0:1);}
