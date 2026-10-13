#include "maicord/text/script.hpp"
#include <Windows.h>
namespace maicord {namespace {
u32 checks=0;
void print(const char* text) noexcept {u32 count=0;while(text[count])++count;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,count,&done,nullptr);}
void number(u32 value) noexcept {char text[16];u32 n=0;do{text[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char v=text[i];text[i]=text[n-i-1];text[n-i-1]=v;}text[n]=0;print(text);}
#define CHECK(value) do{++checks;if(!(value)){print("FAIL script line=");number(__LINE__);print("\n");return false;}}while(0)
bool builtins() noexcept {
    struct Case {u32 scalar;UnicodeScript script;u8 flags;};
    const Case cases[]{
        {'A',UnicodeScript::latn,0},{'0',UnicodeScript::zzzz,native_script_contextual},
        {'(',UnicodeScript::zzzz,native_script_contextual},{0x301,UnicodeScript::zzzz,native_script_contextual},
        {0x1f600,UnicodeScript::zzzz,native_script_contextual},{0x200d,UnicodeScript::zzzz,native_script_contextual},
        {0xfe0f,UnicodeScript::zzzz,native_script_contextual|native_script_selector},{0x180b,UnicodeScript::mong,native_script_contextual|native_script_selector},
        {0x628,UnicodeScript::arab,0},{0x61f,UnicodeScript::arab,0},{0x5b0,UnicodeScript::hebr,0},
        {0x378,UnicodeScript::zzzz,0},{0x1faef,UnicodeScript::zzzz,0},{0x20c1,UnicodeScript::zzzz,0},
        {0x1e100,UnicodeScript::wcho,0},
        {0,UnicodeScript::zyyy,native_script_nonvisual},{9,UnicodeScript::zyyy,native_script_nonvisual},
        {0x200e,UnicodeScript::zyyy,native_script_nonvisual},{0x2028,UnicodeScript::zyyy,native_script_nonvisual},
        {0xe0067,UnicodeScript::zyyy,native_script_nonvisual},
        {0xd800,UnicodeScript::zzzz,native_script_invalid},{0xdfff,UnicodeScript::zzzz,native_script_invalid},
        {0x110000,UnicodeScript::zzzz,native_script_invalid},{0xffffffff,UnicodeScript::zzzz,native_script_invalid}};
    for(const auto& c:cases){const auto result=native_script_class(c.scalar);CHECK(result.script==c.script&&result.flags==c.flags);}
    const u32 input[]{'a','(',0x628,')','b'};NativeScriptClass resolved[8];
    CHECK(native_script_resolve({input,5},{resolved,8})==NativeScriptStatus::ok);
    const UnicodeScript expected[]{UnicodeScript::latn,UnicodeScript::latn,UnicodeScript::arab,UnicodeScript::arab,UnicodeScript::latn};
    for(u32 i=0;i<5;++i)CHECK(resolved[i].script==expected[i]&&!resolved[i].flags);
    const u32 selectors[]{'a',0x200e,0xfe0f,0xfe0f,0x180b,0x4e2d};
    CHECK(native_script_resolve({selectors,6},{resolved,8})==NativeScriptStatus::ok);
    CHECK(resolved[0].script==UnicodeScript::latn&&!resolved[0].flags);
    for(u32 i=1;i<3;++i)CHECK(resolved[i].script==UnicodeScript::zyyy&&resolved[i].flags==native_script_nonvisual);
    for(u32 i=3;i<6;++i)CHECK(resolved[i].script==UnicodeScript::hani&&!resolved[i].flags);
    CHECK(native_script_resolve({}, {})==NativeScriptStatus::ok);
    CHECK(native_script_resolve({nullptr,1},{resolved,8})==NativeScriptStatus::invalid_input);
    CHECK(native_script_resolve({input,5},{resolved,4})==NativeScriptStatus::output_too_small);
    const u32 invalid[]{'a',0xd800};CHECK(native_script_resolve({invalid,2},{resolved,8})==NativeScriptStatus::invalid_input);
    return true;
}
bool fixture(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    struct Release {HANDLE file;~Release() noexcept {CloseHandle(file);}} release{file};
    const auto read=[&](void* p,u32 count) noexcept {DWORD got=0;return ReadFile(file,p,count,&got,nullptr)&&got==count;};
    u32 header[2]{};CHECK(read(header,8));
    u16 values[4096];
    if(header[0]==0x3143534e){u32 contexts=0,context[16];CHECK(header[1]==0x110000&&read(&contexts,4)&&contexts&&contexts<=16&&read(context,contexts*4));
        const u32 stride=contexts*3,batch=4096/stride;
        for(u32 cp=0;cp<header[1];){const u32 count=minimum(batch,header[1]-cp);CHECK(read(values,count*stride*2));
            for(u32 i=0;i<count;++i,++cp)for(u32 c=0;c<contexts;++c){const auto* expected=values+i*stride+c*3;
                if(cp>=0xd800&&cp<=0xdfff){CHECK(expected[0]==0xffff&&expected[1]==0xffff&&expected[2]==0xffff);continue;}
                const u32 scalars[]{context[c],cp,context[c]};NativeScriptClass output[3];const u32 n=context[c]?3u:1u;
                CHECK(native_script_resolve({scalars+(context[c]?0:1),n},{output,n})==NativeScriptStatus::ok);
                for(u32 s=0;s<n;++s){const u16 actual=static_cast<u16>(static_cast<u8>(output[s].script)|(static_cast<u16>(output[s].flags)<<8));
                    if(actual!=expected[s+(context[c]?0:1)]){print("scalar/context/position=");number(cp);print("/");number(c);print("/");number(s);print("\n");}
                    CHECK(actual==expected[s+(context[c]?0:1)]);}}}
    }else if(header[0]==0x3152534e){CHECK(header[1]&&header[1]<=1000000);u32 scalars[4096];NativeScriptClass output[4096];
        for(u32 c=0;c<header[1];++c){u32 count=0;CHECK(read(&count,4)&&count&&count<=4096&&read(scalars,count*4)&&read(values,count*2));
            CHECK(native_script_resolve({scalars,count},{output,count})==NativeScriptStatus::ok);
            for(u32 i=0;i<count;++i){const u16 actual=static_cast<u16>(static_cast<u8>(output[i].script)|(static_cast<u16>(output[i].flags)<<8));
                if(actual!=values[i]){print("case/scalar=");number(c);print("/");number(i);print("\n");}CHECK(actual==values[i]);}}
    }else{CHECK(header[0]==0x3153434e&&header[1]==0x110000);
    for(u32 cp=0;cp<header[1];){const u32 count=minimum<u32>(4096,header[1]-cp);CHECK(read(values,count*2));
        for(u32 i=0;i<count;++i,++cp){const auto value=native_script_class(cp);const u16 packed=static_cast<u16>(static_cast<u8>(value.script)|(static_cast<u16>(value.flags)<<8));
            if(packed!=values[i]){print("scalar=");number(cp);print(" native/expected=");number(packed);print("/");number(values[i]);print("\n");}CHECK(packed==values[i]);}}}
    u8 tail=0;DWORD bytes=0;CHECK(ReadFile(file,&tail,1,&bytes,nullptr)&&!bytes);return true;
}
bool run() noexcept {
    CHECK(builtins());const wchar_t* command=GetCommandLineW();bool quote=false;
    while(*command){if(*command=='"')quote=!quote;else if(!quote&&(*command==' '||*command=='\t'))break;++command;}
    while(*command==' '||*command=='\t')++command;
    if(*command){wchar_t path[32768];u32 count=0;quote=*command=='"';if(quote)++command;
        while(*command&&!(quote?*command=='"':(*command==' '||*command=='\t'))){CHECK(count+1<32768);path[count++]=*command++;}path[count]=0;
        if(quote){CHECK(*command=='"');++command;}while(*command==' '||*command=='\t')++command;CHECK(!*command&&fixture(path));}
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));print("checks=");number(checks);print(" PASS native script classes and resolution\n");return true;
}
}}
extern "C" int maicord_script_tests_entry() noexcept {ExitProcess(maicord::run()?0u:1u);}
