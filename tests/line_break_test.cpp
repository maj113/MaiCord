// Manual: --bench times the line break classifier; the registered run is the check.
#include "maicord/text/line_break.hpp"

namespace maicord {namespace {
#include "generated/line_break_test.inc"
void print(Utf8View text) noexcept {DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data,text.bytes,&done,nullptr);}
void number(u32 value) noexcept {char s[16];u32 n=0;do{s[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){char t=s[i];s[i]=s[n-i-1];s[n-i-1]=t;}print({s,n});}
bool vectors() noexcept {
    u32 text[256];u8 boundaries[257];u32 at=0,cases=0,scalars=0,checks=0,failures=0;
    constexpr u32 size=sizeof(line_vectors)/sizeof(line_vectors[0]);
    while(at<size){
        if(size-at<2)return false;
        const u32 tagged_count=line_vectors[at++],line=line_vectors[at++],count=tagged_count&0x7fffffff;
        if(!count||count>256||count>size-at)return false;
        for(u32 i=0;i<count;++i)text[i]=line_vectors[at+i]&0x1fffff;
        if(!unicode_line_breaks({text,count},{boundaries,count+1}))return false;
        for(u32 i=0;i<=count;++i){const u8 expected=(i<count?line_vectors[at+i]:tagged_count)>>31;
            if((boundaries[i]!=0)!=expected||boundaries[i]>2){
                ++failures;if(failures<=20){print(utf8_literal("line source_line="));number(line);print(utf8_literal(" boundary="));number(i);print(utf8_literal(" expected="));number(expected);print(utf8_literal(" actual="));number(boundaries[i]);print(utf8_literal("\n"));}}
            ++checks;
        }
        at+=count;++cases;scalars+=count;
    }
    print(utf8_literal("line_cases="));number(cases);print(utf8_literal("\nline_scalars="));number(scalars);
    print(utf8_literal("\nline_boundary_checks="));number(checks);print(utf8_literal("\nline_failures="));number(failures);print(utf8_literal("\n"));
    return cases==line_vector_count&&!failures;
}
bool api_cases() noexcept {
    u8 out[32];for(auto& b:out)b=0x5a;
    const u32 text[]{'a'},invalid[]{'a',0xdfff},too_high[]{0x110000};
    if(unicode_line_breaks({invalid,2},{out,32})||unicode_line_breaks({too_high,1},{out,32})||
       unicode_line_breaks({nullptr,1},{out,32})||unicode_line_breaks({text,1},{nullptr,32})||
       unicode_line_breaks({text,1},{out,1})||unicode_line_breaks({text,~0u},{out,~0u}))return false;
    for(u8 b:out)if(b!=0x5a)return false;
    if(!unicode_line_breaks({},{out,32})||out[0])return false;
    for(u32 i=1;i<32;++i)if(out[i]!=0x5a)return false;
    if(!unicode_line_breaks({text,1},{out,32})||out[0]||out[1]!=2)return false;
    for(u32 i=2;i<32;++i)if(out[i]!=0x5a)return false;
    const u32 controls[]{'a',0xd,0xa,'b',0xb,'c',0xc,'d',0x85,'e',0x2028,'f',0x2029,'g'};
    const u8 expected[]{0,0,0,2,0,2,0,2,0,2,0,2,0,2,2};
    if(!unicode_line_breaks({controls,14},{out,32}))return false;
    for(u32 i=0;i<15;++i)if(out[i]!=expected[i])return false;
    // Original ZWJ blocks following EM even after LB10 remaps its properties.
    const u32 zwj[]{' ',0x200d,0x1f3fb};
    if(!unicode_line_breaks({zwj,3},{out,32})||out[0]||out[1]!=1||out[2]||out[3]!=2)return false;
    return true;
}
bool property_contract() noexcept {
    u32 count=0,ideographs=0,unknown=0;
    for(u32 cp=0;cp<=0x10ffff;++cp){const auto& p=*unicode_properties(cp);
        if(p.general_category()==UnicodeGeneralCategory::cn&&p.has(UnicodeFlag::extended_pictographic)){
            if(p.line_break()==UnicodeLineBreak::id)++ideographs;else if(p.line_break()==UnicodeLineBreak::xx)++unknown;else return false;++count;}}
    print(utf8_literal("line_future_emoji_properties="));number(count);print(utf8_literal("\nline_future_emoji_ideographic="));number(ideographs);
    print(utf8_literal("\nline_future_emoji_unknown="));number(unknown);print(utf8_literal("\n"));return count==1453;
}
bool combining_invariance() noexcept {
    // LB9 makes these characters transparent after an eligible first base.
    // Keep the published expected answers at every original boundary instead
    // of using a second run of the implementation as the expected result.
    const u32 marks[]{0x301,0xfe0f,0x200c};
    for(u32 cp:marks)if(unicode_properties(cp)->line_break()!=UnicodeLineBreak::cm)return false;
    u32 text[259];u8 out[260];u32 at=0,cases=0,checks=0;
    constexpr u32 size=sizeof(line_vectors)/sizeof(line_vectors[0]);
    while(at<size){
        const u32 tagged=line_vectors[at++];++at;const u32 n=tagged&0x7fffffff;
        const u32 first=line_vectors[at]&0x1fffff;const auto& p=*unicode_properties(first);const auto b=p.line_break();
        using B=UnicodeLineBreak;using G=UnicodeGeneralCategory;
        const bool excluded=b==B::cm||b==B::zwj||b==B::bk||b==B::cr||b==B::lf||b==B::nl||b==B::sp||b==B::zw||
            (b==B::sa&&(p.general_category()==G::mn||p.general_category()==G::mc));
        if(!excluded){
            if(n>256)return false;text[0]=first;for(u32 i=0;i<3;++i)text[i+1]=marks[i];
            for(u32 i=1;i<n;++i)text[i+3]=line_vectors[at+i]&0x1fffff;
            if(!unicode_line_breaks({text,n+3},{out,n+4}))return false;
            for(u32 i=0;i<4;++i){if(out[i])return false;++checks;}
            for(u32 i=1;i<=n;++i){const u32 expected=(i<n?line_vectors[at+i]:tagged)>>31;
                if((out[i+3]!=0)!=expected){print(utf8_literal("FAIL combining source_line="));number(line_vectors[at-1]);print(utf8_literal(" boundary="));number(i);print(utf8_literal("\n"));return false;}++checks;}
            ++cases;
        }
        at+=n;
    }
    print(utf8_literal("line_combining_cases="));number(cases);print(utf8_literal("\nline_combining_checks="));number(checks);print(utf8_literal("\n"));return cases>0;
}
bool long_contexts() noexcept {
    constexpr u32 size=300003;
    auto* text=static_cast<u32*>(VirtualAlloc(nullptr,size*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* out=static_cast<u8*>(VirtualAlloc(nullptr,size+1,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!text||!out){if(text)VirtualFree(text,0,MEM_RELEASE);if(out)VirtualFree(out,0,MEM_RELEASE);return false;}
    bool ok=true;
    // Numeric lookbehind must be state, not a fresh backwards punctuation scan.
    text[0]='1';for(u32 i=1;i<size-1;++i)text[i]='.';text[size-1]='%';
    ok=unicode_line_breaks({text,size},{out,size+1});
    if(ok)for(u32 i=0;i<=size;++i)if(out[i]!=(i==size?2:0)){ok=false;break;}
    // LB9 removes marks from RI parity, not from raw boundary output.
    for(u32 i=0;i<size;++i)text[i]=i%3==0?0x1f1e6:0x301;
    if(ok)ok=unicode_line_breaks({text,size},{out,size+1});
    if(ok)for(u32 i=0;i<=size;++i)if(out[i]!=(i==size?2:(i&&i%6==0?1:0))){ok=false;break;}
    // A quote remembers its preceding context across both ignored marks and SP*.
    text[0]=0x20;text[1]=0x201c;
    for(u32 i=2;i<100002;++i)text[i]=0x301;
    for(u32 i=100002;i<size-1;++i)text[i]=0x20;text[size-1]='a';
    if(ok)ok=unicode_line_breaks({text,size},{out,size+1});
    if(ok)for(u32 i=0;i<=size;++i)if(out[i]!=(i==size?2:(i==1?1:0))){ok=false;break;}
    // Repeated separate lookaheads, including OP IS NU through marks.
    const u32 pattern[]{'$', '(',0x301,'.',0x301,'1',')','%',0x20};
    for(u32 i=0;i<size;++i)text[i]=pattern[i%9];
    if(ok)ok=unicode_line_breaks({text,size},{out,size+1});
    if(ok)for(u32 i=0;i<=size;++i)if(out[i]!=(i==size?2:(i&&i%9==0?1:0))){ok=false;break;}
    VirtualFree(out,0,MEM_RELEASE);VirtualFree(text,0,MEM_RELEASE);return ok;
}
bool protected_pages() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* source=static_cast<u8*>(VirtualAlloc(nullptr,page*3,MEM_RESERVE,PAGE_NOACCESS));
    auto* dest=static_cast<u8*>(VirtualAlloc(nullptr,page*3,MEM_RESERVE,PAGE_NOACCESS));
    bool ok=source&&dest;
    if(ok)ok=VirtualAlloc(source+page,page,MEM_COMMIT,PAGE_READWRITE)&&VirtualAlloc(dest+page,page,MEM_COMMIT,PAGE_READWRITE);
    if(ok){
        const u32 counts[]{0,1,2,3,127,199};
        for(u32 n:counts)for(u32 side=0;side<2&&ok;++side){
            auto* input=reinterpret_cast<u32*>(source+page+(side?page-n*4:0));
            auto* output=dest+page+(side?page-n-1:0);DWORD old=0;
            for(u32 i=0;i<n;++i)input[i]='a';for(u32 i=0;i<=n;++i)output[i]=0x5a;
            ok=VirtualProtect(source+page,page,PAGE_READONLY,&old)&&unicode_line_breaks({input,n},{output,n+1});
            if(ok)for(u32 i=0;i<=n;++i)if(output[i]!=(i==n&&n?2:0)){ok=false;break;}
            if(!VirtualProtect(source+page,page,PAGE_READWRITE,&old))ok=false;
        }
    }
    if(dest)VirtualFree(dest,0,MEM_RELEASE);if(source)VirtualFree(source,0,MEM_RELEASE);return ok;
}
bool benchmark() noexcept {
    constexpr u32 capacity=65536;
    auto* text=static_cast<u32*>(VirtualAlloc(nullptr,capacity*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* out=static_cast<u8*>(VirtualAlloc(nullptr,capacity+1,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    struct Release {u32* text;u8* out;~Release() noexcept {if(out)VirtualFree(out,0,MEM_RELEASE);if(text)VirtualFree(text,0,MEM_RELEASE);}} release{text,out};
    if(!text||!out)return false;LARGE_INTEGER frequency{};
    if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0||frequency.QuadPart>0xffffffffll)return false;
    const u32 latin[]{'C','a','f',0xe9,' ',0x201c,'a',0x301,' ','b',0x201d,' ','1','.','5','%',' '};
    const u32 mixed[]{0x65e5,0x672c,0x300c,0x25cc,0x301,0x1f1e6,0x301,0x1f1e7,' ',0x1f469,0x200d,0x1f4bb,' ',0x1100,0x1161,0x11a8,' ',0x11f12,0x11f42,0x11f13};
    const u32 sizes[]{64,1024,65536};
    for(u32 sample=0;sample<2;++sample)for(u32 n:sizes){
        const u32* pattern=sample?mixed:latin;const u32 length=sample?sizeof(mixed)/4:sizeof(latin)/4;
        for(u32 i=0;i<n;++i)text[i]=pattern[i%length];
        if(!unicode_line_breaks({text,n},{out,n+1}))return false;
        u32 expected=2166136261u;for(u32 i=0;i<=n;++i)expected=(expected^out[i])*16777619u;
        const u32 iterations=1048576/n;
        print(utf8_literal("{\"kind\":\"line_break_benchmark\",\"sample\":"));number(sample);print(utf8_literal(",\"scalars\":"));number(n);
        print(utf8_literal(",\"iterations\":"));number(iterations);print(utf8_literal(",\"frequency\":"));number(static_cast<u32>(frequency.QuadPart));print(utf8_literal(",\"digest\":"));number(expected);print(utf8_literal(",\"batches\":["));
        for(u32 batch=0;batch<3;++batch){LARGE_INTEGER start{},end{};if(!QueryPerformanceCounter(&start))return false;
            for(u32 i=0;i<iterations;++i)if(!unicode_line_breaks({text,n},{out,n+1}))return false;
            if(!QueryPerformanceCounter(&end)||end.QuadPart<start.QuadPart||end.QuadPart-start.QuadPart>0xffffffffll)return false;
            u32 actual=2166136261u;for(u32 i=0;i<=n;++i)actual=(actual^out[i])*16777619u;if(actual!=expected)return false;
            if(batch)print(utf8_literal(","));number(static_cast<u32>(end.QuadPart-start.QuadPart));
        }
        print(utf8_literal("]}\n"));
    }
    return true;
}
int run() noexcept {
    const wchar_t* command=GetCommandLineW();bool quote=false;
    while(*command){if(*command=='"')quote=!quote;else if(!quote&&(*command==' '||*command=='\t'))break;++command;}while(*command==' '||*command=='\t')++command;
    if(*command){constexpr wchar_t option[]=L"--bench";u32 n=0;while(option[n]&&command[n]==option[n])++n;if(n!=7||command[n])return 6;return benchmark()?0:7;}
    if(!vectors())return 1;
    if(!api_cases()){print(utf8_literal("FAIL line API cases\n"));return 2;}
    if(!property_contract()){print(utf8_literal("FAIL line property contract\n"));return 8;}
    if(!combining_invariance())return 5;
    if(!long_contexts()){print(utf8_literal("FAIL line long contexts\n"));return 3;}
    if(!protected_pages()){print(utf8_literal("FAIL line protected pages\n"));return 4;}
    print(utf8_literal("PASS native default line breaks\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_line_break_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
