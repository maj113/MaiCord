// QA only: compare scalar-native paragraph and line levels with the installed
// DirectWrite analyzer. No font, layout or raster comparison is implied.
#include "maicord/text/bidi.hpp"
#include "../../src/text/bidi_internal.hpp"
#include "maicord/core/byte_order.hpp"
#include <dwrite.h>

namespace maicord { namespace {
constexpr u32 capacity=512;
UnicodeBidiCell cells[capacity];u32 scratch[capacity],starts[capacity+1];u8 levels[capacity],expected_levels[capacity],layout_levels[capacity];
char output[65536];u32 output_size=0;bool output_ok=true;
void flush() noexcept {DWORD done=0;if(output_size && (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),output,output_size,&done,nullptr) || done!=output_size))output_ok=false;output_size=0;}
void print(Utf8View text) noexcept {for(u32 i=0;i<text.bytes;++i){if(output_size==sizeof(output))flush();output[output_size++]=text.data[i];}}
void number(u32 value) noexcept {char s[16];u32 n=0;do{s[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){char t=s[i];s[i]=s[n-i-1];s[n-i-1]=t;}print({s,n});}
void field(u32 value) noexcept {print(utf8_literal(","));number(value);}
void vector(ConstSlice<u8> values) noexcept {print(utf8_literal(","));for(u32 i=0;i<values.count;++i){if(i)print(utf8_literal(" "));number(values[i]);}}
struct Analysis final : IDWriteTextAnalysisSource,IDWriteTextAnalysisSink {
    wchar_t text[capacity*2];u8 explicit_levels[capacity*2],resolved[capacity*2],seen[capacity*2],run_starts[capacity*2];u32 length=0;u8 base=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID)) || bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSource),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSource*>(this);
        else if(bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSink),sizeof(GUID)))*out=static_cast<IDWriteTextAnalysisSink*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 at,WCHAR const** value,UINT32* count) override {
        *value=at<length?text+at:nullptr;*count=at<length?length-at:0;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 at,WCHAR const** value,UINT32* count) override {
        if(at>length)return E_INVALIDARG;*value=at?text:nullptr;*count=at;return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {return base?DWRITE_READING_DIRECTION_RIGHT_TO_LEFT:DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;}
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 at,UINT32* count,WCHAR const** value) override {
        if(at>length)return E_INVALIDARG;*count=length-at;*value=L"en-US";return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 at,UINT32* count,IDWriteNumberSubstitution** value) override {
        if(at>length)return E_INVALIDARG;*count=length-at;*value=nullptr;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32,UINT32,DWRITE_SCRIPT_ANALYSIS const*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32,UINT32,DWRITE_LINE_BREAKPOINT const*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32,UINT32,IDWriteNumberSubstitution*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32 at,UINT32 count,UINT8 explicit_level,UINT8 implicit_level) override {
        if(at>length || count>length-at)return E_INVALIDARG;
        for(u32 i=at;i<at+count;++i){explicit_levels[i]=explicit_level;resolved[i]=implicit_level;seen[i]=1;run_starts[i]=i==at;}return S_OK;
    }
};
struct Reader {
    const u8* data;u32 size,at;
    bool word(u32* value) noexcept {if(size-at<4)return false;*value=byte_load_le32(data+at);at+=4;return true;}
    bool take(u32 count,const u8** value) noexcept {if(count>size-at)return false;*value=data+at;at+=count;return true;}
};
bool compare(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size) || size.QuadPart<12 || size.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping){CloseHandle(file);return false;}
    auto* bytes=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));
    IDWriteFactory* factory=nullptr;IDWriteTextAnalyzer* analyzer=nullptr;IDWriteTextFormat* format=nullptr;
    const bool expected=bytes && bytes_equal(bytes,"MCBIDI1",8);
    const bool probe=bytes && bytes_equal(bytes,"MCBIDIP",8);
    bool ok=(expected || probe) && SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory))) && SUCCEEDED(factory->CreateTextAnalyzer(&analyzer));
    Reader reader{bytes,static_cast<u32>(size.QuadPart),8};u32 total=0;if(ok)ok=reader.word(&total) && total>0;
    if(ok)ok=SUCCEEDED(factory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16.0f,L"en-US",&format)) && SUCCEEDED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
    Analysis analysis;
    print(utf8_literal("source_line,policy,base,scalars,checked,removed,explicit_diff,paragraph_diff,line_diff,surrogate_diff,first_diff,dw_level,native_paragraph,native_line,layout_diff,layout_analyzer_diff,first_layout_diff,layout_level,dw_levels,native_levels,line_levels,layout_levels,has_expected,candidate_diff,candidate_levels,candidate_retained,candidate_removed,candidate_isolate_removed,candidate_retained_diff,candidate_base,dw_run_starts,dw_explicit_levels\n"));
    for(u32 test=0;test<total && ok;++test) {
        u32 count=0,policy=0,expected_base=0,visual_count=0,line=0,kind=0;
        ok=reader.word(&count)&&reader.word(&policy)&&reader.word(&expected_base)&&reader.word(&visual_count)&&reader.word(&line)&&reader.word(&kind)&&count<=capacity&&policy<=2&&kind<=1&&visual_count<=count;
        if(!ok)break;
        if(probe && (kind!=1 || policy>1 || expected_base!=255 || visual_count)){ok=false;break;}
        analysis.length=0;
        for(u32 i=0;i<count && ok;++i) {
            u32 cp;const u8* record=nullptr;ok=reader.word(&cp)&&reader.take(4,&record)&&unicode_is_scalar(cp);if(!ok)break;
            if(kind && (record[0]!=255 || (expected && expected_base>1) || (probe && record[1]!=255))){ok=false;break;}expected_levels[i]=record[1];
            cells[i]={cp,0,0,unicode_properties(cp)->bidi_class(),{},0,0};starts[i]=analysis.length;
            if(cp<=0xffff)analysis.text[analysis.length++]=static_cast<wchar_t>(cp);
            else{cp-=0x10000;analysis.text[analysis.length++]=static_cast<wchar_t>(0xd800+(cp>>10));analysis.text[analysis.length++]=static_cast<wchar_t>(0xdc00+(cp&1023));}
        }
        const u8* ignored=nullptr;if(ok)ok=reader.take(visual_count*4,&ignored);if(!ok)break;
        if(!kind)continue; // Abstract classes have no corresponding source text.
        starts[count]=analysis.length;UnicodeBidiParagraph paragraph{};u32 ordered=0;
        ok=unicode_bidi_resolve({cells,count},static_cast<UnicodeParagraphDirection>(policy),{scratch,count},&paragraph)==UnicodeBidiStatus::ok &&
           unicode_bidi_line({cells,count},paragraph.level,0,count,{levels,count},{scratch,count},&ordered)==UnicodeBidiStatus::ok;
        if(!ok)break;
        if(expected) {
            if(paragraph.level!=expected_base || ordered!=visual_count){ok=false;break;}
            for(u32 i=0;i<count;++i)if(levels[i]!=expected_levels[i])ok=false;
            for(u32 i=0;i<ordered;++i)if(scratch[i]!=byte_load_le32(ignored+i*4))ok=false;
        }
        if(!ok)break;
        // DirectWrite exposes forced paragraph direction only. For auto cases,
        // supply the corpus-verified native base and label this policy in CSV.
        analysis.base=paragraph.level;for(u32 i=0;i<analysis.length;++i)analysis.seen[i]=0;
        ok=SUCCEEDED(analyzer->AnalyzeBidi(&analysis,0,analysis.length,&analysis));if(!ok)break;
        for(u32 i=0;i<analysis.length;++i)if(!analysis.seen[i])ok=false;if(!ok)break;
        u32 checked=0,removed=0,ed=0,pd=0,ld=0,sd=0,first=~0u;
        for(u32 i=0;i<count;++i) {
            const u32 at=starts[i];
            if(starts[i+1]-at==2 && (analysis.explicit_levels[at]!=analysis.explicit_levels[at+1] || analysis.resolved[at]!=analysis.resolved[at+1]))++sd;
            if(cells[i].level==unicode_bidi_removed){++removed;continue;}
            ++checked;ed+=analysis.explicit_levels[at]!=cells[i].explicit_level;pd+=analysis.resolved[at]!=cells[i].level;ld+=analysis.resolved[at]!=levels[i];
            if(first==~0u && analysis.resolved[at]!=levels[i])first=i;
        }
        IDWriteTextLayout* layout=nullptr;u32 layout_diff=0,layout_analyzer_diff=0,first_layout=~0u,layout_level=255;
        ok=SUCCEEDED(format->SetReadingDirection(analysis.GetParagraphReadingDirection())) && SUCCEEDED(factory->CreateTextLayout(analysis.text,analysis.length,format,65536.0f,65536.0f,&layout));
        for(u32 i=0;i<count && ok;++i) {
            layout_levels[i]=255;
            if(cells[i].level==unicode_bidi_removed)continue;
            float x=0,y=0;DWRITE_HIT_TEST_METRICS hit{};
            ok=SUCCEEDED(layout->HitTestTextPosition(starts[i],FALSE,&x,&y,&hit));if(!ok)break;
            if(hit.bidiLevel>126){ok=false;break;}layout_levels[i]=static_cast<u8>(hit.bidiLevel);
            layout_diff+=hit.bidiLevel!=levels[i];layout_analyzer_diff+=hit.bidiLevel!=analysis.resolved[starts[i]];
            if(first_layout==~0u && hit.bidiLevel!=levels[i]){first_layout=i;layout_level=hit.bidiLevel;}
        }
        if(layout)layout->Release();if(!ok)break;
        number(line);field(policy);field(paragraph.level);field(count);field(checked);field(removed);field(ed);field(pd);field(ld);field(sd);field(first);
        field(first==~0u?255:analysis.resolved[starts[first]]);field(first==~0u?255:cells[first].level);field(first==~0u?255:levels[first]);
        field(layout_diff);field(layout_analyzer_diff);field(first_layout);field(layout_level);
        u8 dw[capacity],native[capacity];for(u32 i=0;i<count;++i){dw[i]=analysis.resolved[starts[i]];native[i]=cells[i].level;}
        vector({dw,count});vector({native,count});vector({levels,count});vector({layout_levels,count});field(expected?1:0);
        UnicodeBidiParagraph candidate{};u8 candidate_levels[capacity];u32 candidate_diff=0;
        // Windows exposes forced direction only. Use the same captured base for
        // both sides, including standard-corpus cases with an auto policy.
        ok=bidi_windows_resolve_candidate({cells,count},static_cast<UnicodeParagraphDirection>(analysis.base),{scratch,count},&candidate)==UnicodeBidiStatus::ok &&
           unicode_bidi_line({cells,count},candidate.level,0,count,{candidate_levels,count},{scratch,count},&ordered)==UnicodeBidiStatus::ok;
        if(!ok)break;
        u32 retained=0,candidate_removed=0,isolate_removed=0,retained_diff=0;
        for(u32 i=0;i<count;++i) {
            const bool isolate_control=cells[i].scalar>=0x2066 && cells[i].scalar<=0x2069;
            const bool excluded=levels[i]==255 || isolate_control;
            // The new metric cannot hide unexpected source exclusion. Preserve
            // the old raw counter, including differing control marker values.
            if((candidate_levels[i]==255)!=excluded || cells[i].original!=unicode_properties(cells[i].scalar)->bidi_class()){ok=false;break;}
            if(levels[i]!=255 && candidate_levels[i]!=dw[i])++candidate_diff;
            if(excluded){++candidate_removed;isolate_removed+=isolate_control;}
            else{++retained;retained_diff+=candidate_levels[i]!=dw[i];}
        }
        if(!ok || ordered!=retained){ok=false;break;}
        field(candidate_diff);vector({candidate_levels,count});field(retained);field(candidate_removed);field(isolate_removed);field(retained_diff);field(candidate.level);
        for(u32 i=0;i<count;++i){dw[i]=analysis.run_starts[starts[i]];native[i]=analysis.explicit_levels[starts[i]];}vector({dw,count});vector({native,count});print(utf8_literal("\n"));
    }
    if(ok)ok=reader.at==reader.size;
    if(format)format->Release();if(analyzer)analyzer->Release();if(factory)factory->Release();if(bytes)UnmapViewOfFile(bytes);CloseHandle(mapping);CloseHandle(file);return ok;
}
int run() noexcept {
    const wchar_t* command=GetCommandLineW();bool quote=false;
    while(*command){if(*command=='"')quote=!quote;else if(!quote && (*command==' ' || *command=='\t'))break;++command;}
    while(*command==' ' || *command=='\t')++command;
    wchar_t path[32768];u32 count=0;quote=*command=='"';if(quote)++command;
    while(*command && !(quote?*command=='"':(*command==' ' || *command=='\t'))){if(count+1>=32768)return 2;path[count++]=*command++;}
    if(quote){if(*command!='"')return 2;++command;}while(*command==' ' || *command=='\t')++command;
    if(*command || !count)return 2;path[count]=0;const bool ok=compare(path);flush();return ok && output_ok?0:1;
}
} }
extern "C" __declspec(noreturn) void WINAPI maicord_bidi_reference_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
