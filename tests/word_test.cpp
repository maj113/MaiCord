#include "maicord/text/word.hpp"
#include "maicord/core/utf.hpp"
#include <Windows.h>

namespace maicord {namespace {
#include "generated/word_break_test.inc"
void print(Utf8View text) noexcept {DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data,text.bytes,&done,nullptr);}
void number(u32 value) noexcept {char s[16];u32 n=0;do{s[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){char t=s[i];s[i]=s[n-i-1];s[n-i-1]=t;}print({s,n});}
bool vectors() noexcept {
    constexpr u32 capacity=64;u32 text[capacity];u8 boundaries[capacity+1];u32 at=0,cases=0,scalars=0,checks=0,failures=0;
    constexpr u32 size=sizeof(word_vectors)/sizeof(word_vectors[0]);
    while(at<size) {
        if(size-at<2)return false;
        const u32 tagged_count=word_vectors[at++],line=word_vectors[at++],count=tagged_count&0x7fffffff;
        if(!count || count>capacity || count>size-at)return false;
        for(u32 i=0;i<count;++i)text[i]=word_vectors[at+i]&0x1fffff;
        if(!unicode_word_boundaries({text,count},{boundaries,count+1}))return false;
        for(u32 i=0;i<=count;++i) {
            const u8 expected=(i<count?word_vectors[at+i]:tagged_count)>>31;
            if(boundaries[i]!=expected) {
                ++failures;
                if(failures<=20){print(utf8_literal("word source_line="));number(line);print(utf8_literal(" boundary="));number(i);print(utf8_literal(" expected="));number(expected);print(utf8_literal(" actual="));number(boundaries[i]);print(utf8_literal("\n"));}
            }
            ++checks;
        }
        at+=count;++cases;scalars+=count;
    }
    print(utf8_literal("word_cases="));number(cases);print(utf8_literal("\nword_scalars="));number(scalars);
    print(utf8_literal("\nword_boundary_checks="));number(checks);print(utf8_literal("\nword_failures="));number(failures);print(utf8_literal("\n"));
    return cases==word_vector_count && !failures;
}
bool api_cases() noexcept {
    u8 output[10];for(auto& value:output)value=0x5a;
    const u32 valid[]{'a'},invalid[]{'a',0xd800},too_high[]{0x110000};
    if(unicode_word_boundaries({invalid,2},{output,10}) || unicode_word_boundaries({too_high,1},{output,10}) ||
       unicode_word_boundaries({nullptr,1},{output,10}) || unicode_word_boundaries({valid,1},{nullptr,10}) ||
       unicode_word_boundaries({valid,1},{output,1}) || unicode_word_boundaries({valid,~0u},{output,~0u}))return false;
    for(const auto value:output)if(value!=0x5a)return false;
    if(!unicode_word_boundaries({},{output,10}) || output[0])return false;
    for(u32 i=1;i<10;++i)if(output[i]!=0x5a)return false;
    if(!unicode_word_boundaries({valid,1},{output,10}) || output[0]!=1 || output[1]!=1)return false;
    for(u32 i=2;i<10;++i)if(output[i]!=0x5a)return false;
    const u32 spaces[]{0x20,0x301,0x20};
    if(!unicode_word_boundaries({spaces,3},{output,10}) || output[0]!=1 || output[1]!=0 || output[2]!=1 || output[3]!=1)return false;
    const u32 leading[]{0x200e,'a'};
    if(!unicode_word_boundaries({leading,2},{output,10}) || output[0]!=1 || output[1]!=1 || output[2]!=1)return false;
    const u32 emoji[]{0x1f469,0x200d,0x1f4bb};
    if(!unicode_word_boundaries({emoji,3},{output,10}) || output[0]!=1 || output[1]!=0 || output[2]!=0 || output[3]!=1)return false;
    // Preserve the distinction between the app's current selection policy and
    // UAX #29. Wiring the new default directly into navigation would change it.
    u32 first=0,last=0;const u32 contraction[]{'c','a','n','\'','t'};
    if(!utf8_word_range(utf8_literal("can't"),0,&first,&last) || first || last!=3 ||
       !unicode_word_boundaries({contraction,5},{output,10}))return false;
    for(u32 i=0;i<=5;++i)if(output[i]!=(i==0 || i==5))return false;
    const u32 mixed[]{0x4e2d,0x1f642,0x6587};
    if(!utf8_word_range(utf8_literal("\xe4\xb8\xad\xf0\x9f\x99\x82\xe6\x96\x87"),3,&first,&last) || first!=3 || last!=7 ||
       !unicode_word_boundaries({mixed,3},{output,10}))return false;
    for(u32 i=0;i<=3;++i)if(output[i]!=1)return false;
    return true;
}
bool long_contexts() noexcept {
    constexpr u32 capacity=300003;
    auto* text=static_cast<u32*>(VirtualAlloc(nullptr,capacity*sizeof(u32),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* output=static_cast<u8*>(VirtualAlloc(nullptr,capacity+1,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!text || !output){if(text)VirtualFree(text,0,MEM_RELEASE);if(output)VirtualFree(output,0,MEM_RELEASE);return false;}
    bool ok=true;
    constexpr u32 depth=100000,count=depth*2+3;
    text[0]='a';for(u32 i=1;i<=depth;++i)text[i]=0x301;text[depth+1]='\'';
    for(u32 i=depth+2;i<count-1;++i)text[i]=0x200e;text[count-1]='b';
    ok=unicode_word_boundaries({text,count},{output,capacity+1});
    if(ok)for(u32 i=0;i<=count;++i)if(output[i]!=(i==0 || i==count)){ok=false;break;}
    // Raw newline precedence and the initial ignored run must remain visible.
    text[0]=0xa;for(u32 i=1;i<count-1;++i)text[i]=0x301;text[count-1]='a';
    if(ok)ok=unicode_word_boundaries({text,count},{output,capacity+1});
    if(ok)for(u32 i=0;i<=count;++i)if(output[i]!=(i==0 || i==1 || i==count-1 || i==count)){ok=false;break;}
    for(u32 i=0;i<capacity;++i)text[i]=i%3==0?0x1f1e6:i%3==1?0x301:0x200e;
    if(ok)ok=unicode_word_boundaries({text,capacity},{output,capacity+1});
    if(ok)for(u32 i=0;i<=capacity;++i)if(output[i]!=(i==capacity || i%6==0)){ok=false;break;}
    // Many separate lookaheads, not only one very long ignored suffix.
    for(u32 i=0;i<capacity;++i)text[i]=i%3==0?'a':i%3==1?':':0x301;text[capacity-1]='a';
    if(ok)ok=unicode_word_boundaries({text,capacity},{output,capacity+1});
    if(ok)for(u32 i=0;i<=capacity;++i)if(output[i]!=(i==0 || i==capacity)){ok=false;break;}
    VirtualFree(output,0,MEM_RELEASE);VirtualFree(text,0,MEM_RELEASE);return ok;
}
int run() noexcept {
    if(!vectors())return 1;
    if(!api_cases()){print(utf8_literal("FAIL word API cases\n"));return 2;}
    if(!long_contexts()){print(utf8_literal("FAIL word long contexts\n"));return 3;}
    print(utf8_literal("PASS native default word boundaries\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_word_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
