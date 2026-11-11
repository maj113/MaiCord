#include "maicord/text/unicode/script.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../support/test_output.hpp"
namespace maicord {namespace {
u32 checks=0;
void print(const char* text) noexcept {u32 count=0;while(text[count])++count;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,count,&done,nullptr);}
void number(u32 value) noexcept {char text[16];u32 n=0;do{text[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char v=text[i];text[i]=text[n-i-1];text[n-i-1]=v;}text[n]=0;print(text);}
#define CHECK(value) do{++checks;if(!(value)){print("FAIL script line=");number(__LINE__);print("\n");return false;}}while(0)
bool builtins() noexcept {
    // Unicode 17.0 Script values (UAX #24): Arabic question mark and the Saudi
    // riyal sign are Common, U+1E100 is Nyiakeng Puachue Hmong.
    struct Case {u32 scalar;UnicodeScript script;u8 flags;};
    const Case cases[]{
        {'A',UnicodeScript::latn,0},{'0',UnicodeScript::zzzz,native_script_contextual},
        {'(',UnicodeScript::zzzz,native_script_contextual},{0x301,UnicodeScript::zzzz,native_script_contextual},
        {0x1f600,UnicodeScript::zzzz,native_script_contextual},{0x200d,UnicodeScript::zzzz,native_script_contextual},
        {0xfe0f,UnicodeScript::zzzz,native_script_contextual|native_script_selector},{0x180b,UnicodeScript::mong,native_script_contextual|native_script_selector},
        {0x628,UnicodeScript::arab,0},{0x61f,UnicodeScript::zzzz,native_script_contextual},{0x5b0,UnicodeScript::hebr,0},
        {0x378,UnicodeScript::zzzz,0},{0x20c1,UnicodeScript::zzzz,native_script_contextual},
        {0x1e100,UnicodeScript::hmnp,0},
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
bool run() noexcept {
    CHECK(builtins());
    print("checks=");number(checks);print(" PASS native script classes and resolution\n");return true;
}
}}
extern "C" int maicord_script_tests_entry() noexcept {::maicord::test_process_exit(maicord::run()?0u:1u);}
