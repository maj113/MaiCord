// QA only: prepared-input script analysis cost, excluding UTF decoding/font work.
#define maicord_script_analysis_reference_entry maicord_unused_script_analysis_entry
#include "script_analysis_reference.cpp"
#undef maicord_script_analysis_reference_entry
#include "maicord/text/script.hpp"
namespace maicord {namespace {
volatile u32 checksum=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 n) noexcept {char s[32];u32 size=0;do{s[size++]=static_cast<char>('0'+n%10);n/=10;}while(n);for(u32 i=0;i<size/2;++i){const char c=s[i];s[i]=s[size-i-1];s[size-i-1]=c;}s[size]=0;print(s);}
__declspec(noinline) bool native_call(const u32* text,u32 count,NativeScriptClass* result) noexcept {
    return native_script_resolve({text,count},{result,count})==NativeScriptStatus::ok;
}
bool benchmark(IDWriteTextAnalyzer1* analyzer) noexcept {
    constexpr u32 ascii[]{'T','h','e',' ','q','u','i','c','k',' ','b','r','o','w','n',' ','f','o','x',' ','1','2','3','.',' '};
    constexpr u32 latin[]{'C','a','f',0xe9,' ',0x41,0x301,' ',0x141,'o','d','z',0x307,',',' '};
    constexpr u32 emoji[]{'A',0x1f600,'B',' ',0x1f468,0x200d,0x1f469,0x200d,0x1f467,' ',0x2764,0xfe0f,' '};
    constexpr u32 mixed[]{'A','(',0x628,0x64b,0x629,')',' ',0x5d0,0x5b0,0x5d1,' ',0x410,0x411,0x430,' '};
    constexpr u32 asian[]{0x4e2d,0x56fd,0x3001,0x3042,0x30a2,' ',0x915,0x93f,' ',0xe01,0xe32,' ',0xac00,' '};
    constexpr u32 controls[]{' ',0xfe0f,0xfe0f,0x180b,0x628,0,0xfe0f,0x378,0x1f600,' ',0x61,0x200e,0x301,' '};
    const ConstSlice<u32> samples[]{{ascii,sizeof(ascii)/4},{latin,sizeof(latin)/4},{emoji,sizeof(emoji)/4},{mixed,sizeof(mixed)/4},{asian,sizeof(asian)/4},{controls,sizeof(controls)/4}};
    constexpr u32 lengths[]{16,256,2048};LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency))return false;
    u32 scalars[2048];NativeScriptClass result[2048];Analysis<4096> input;
    for(u32 sample=0;sample<sizeof(samples)/sizeof(samples[0]);++sample)for(u32 count:lengths){
        input.length=0;for(u32 i=0;i<count;++i){const u32 cp=samples[sample][i%samples[sample].count];scalars[i]=cp;
            if(cp>65535){input.text[input.length++]=static_cast<wchar_t>(0xd800+((cp-0x10000)>>10));input.text[input.length++]=static_cast<wchar_t>(0xdc00+((cp-0x10000)&1023));}
            else input.text[input.length++]=static_cast<wchar_t>(cp);}
        const auto call=[&](bool native) noexcept {const bool okay=native?native_call(scalars,count,result):SUCCEEDED(analyzer->AnalyzeScript(&input,0,input.length,&input));
            if(okay)checksum=checksum+static_cast<u32>(native?static_cast<u8>(result[count/2].script):input.scripts[input.length/2].script);return okay;};
        for(u32 warm=0;warm<64;++warm)if(!call(true)||!call(false))return false;
        for(u32 i=0,unit=0;i<count;++i){DWRITE_SCRIPT_PROPERTIES p{};const auto expected=input.scripts[unit];
            if(FAILED(analyzer->GetScriptProperties(expected,&p)))return false;const u32 tag=p.isoScriptCode;
            const u32 iso=(tag<<24)|((tag&0xff00u)<<8)|((tag&0xff0000u)>>8)|(tag>>24);
            if(unicode_script_tag(result[i].script)!=iso||(result[i].flags==native_script_nonvisual)!=(expected.shapes==DWRITE_SCRIPT_SHAPES_NO_VISUAL))return false;
            unit+=scalars[i]>65535?2u:1u;}
        print("{\"sample\":");number(sample);print(",\"scalars\":");number(count);print(",\"utf16_units\":");number(input.length);
        print(",\"frequency\":");number(frequency.QuadPart);print(",\"iterations\":2048,\"batches\":[");
        for(u32 batch=0;batch<12;++batch){u64 ticks[2]{};for(u32 order=0;order<2;++order){const u32 kind=(batch+order)&1;LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);
                for(u32 repeat=0;repeat<2048;++repeat)if(!call(kind==0))return false;QueryPerformanceCounter(&end);ticks[kind]=end.QuadPart-start.QuadPart;}
            if(batch)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print("]");}
        print("]}\n");}
    print("PASS prepared script analysis benchmark\n");return true;
}
int benchmark_run() noexcept {
    IDWriteFactory* factory=nullptr;IDWriteTextAnalyzer* base=nullptr;IDWriteTextAnalyzer1* analyzer=nullptr;
    auto hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateTextAnalyzer(&base);if(SUCCEEDED(hr))hr=base->QueryInterface(__uuidof(IDWriteTextAnalyzer1),reinterpret_cast<void**>(&analyzer));
    const bool okay=SUCCEEDED(hr)&&benchmark(analyzer);if(analyzer)analyzer->Release();if(base)base->Release();if(factory)factory->Release();return okay?0:1;
}
}}
extern "C" int maicord_script_benchmark_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::benchmark_run()));}
