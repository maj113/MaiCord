#include "maicord/text/unicode/script.hpp"
#include "maicord/text/unicode/unicode.hpp"
#include "../../support/test_output.hpp"
namespace maicord {namespace {
u32 checks=0;
constexpr auto& print=test_write_text;constexpr auto& number=test_write_u64;
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
    // The record form gives the same class.
    for(const auto& c:cases)if(!(c.flags&native_script_invalid)){const auto result=native_script_class(c.scalar,*unicode_properties(c.scalar));CHECK(result.script==c.script&&result.flags==c.flags);}
    NativeScriptClass resolved[8];
    const auto classify=[&](const u32* scalars,u32 count) noexcept {for(u32 i=0;i<count;++i)resolved[i]=native_script_class(scalars[i]);};
    const u32 input[]{'a','(',0x628,')','b'};classify(input,5);native_script_resolve({resolved,5});
    const UnicodeScript expected[]{UnicodeScript::latn,UnicodeScript::latn,UnicodeScript::arab,UnicodeScript::arab,UnicodeScript::latn};
    for(u32 i=0;i<5;++i)CHECK(resolved[i].script==expected[i]&&!resolved[i].flags);
    const u32 selectors[]{'a',0x200e,0xfe0f,0xfe0f,0x180b,0x4e2d};classify(selectors,6);native_script_resolve({resolved,6});
    CHECK(resolved[0].script==UnicodeScript::latn&&!resolved[0].flags);
    for(u32 i=1;i<3;++i)CHECK(resolved[i].script==UnicodeScript::zyyy&&resolved[i].flags==native_script_nonvisual);
    for(u32 i=3;i<6;++i)CHECK(resolved[i].script==UnicodeScript::hani&&!resolved[i].flags);
    native_script_resolve({});
    // Contextual cells alone resolve to the unknown script.
    const u32 neutral[]{'0','(',0x301};classify(neutral,3);native_script_resolve({resolved,3});
    for(u32 i=0;i<3;++i)CHECK(resolved[i].script==UnicodeScript::zzzz&&!resolved[i].flags);
    return true;
}
bool run() noexcept {
    CHECK(builtins());
    print("checks=");number(checks);print(" PASS native script classes and resolution\n");return true;
}
}}
extern "C" int maicord_script_tests_entry() noexcept {::maicord::test_process_exit(maicord::run()?0u:1u);}
