#pragma once
#include "maicord/text/generated/unicode_data.hpp"
#include "maicord/text/sfnt.hpp"
namespace maicord {
// Scripts shaped as nominal glyph runs through the alphabetic profile: no
// joining, reordering or required contextual forms. Bidi supplies direction.
// The order fixes native profile IDs; append only.
struct SimpleScript {UnicodeScript script;u32 tag;};
consteval SimpleScript simple_script(UnicodeScript script,const char (&tag)[5]) noexcept {return {script,sfnt_tag(tag[0],tag[1],tag[2],tag[3])};}
inline constexpr SimpleScript simple_scripts[]{
    simple_script(UnicodeScript::bamu,"bamu"),simple_script(UnicodeScript::yiii,"yi  "),simple_script(UnicodeScript::vaii,"vai "),
    simple_script(UnicodeScript::tfng,"tfng"),simple_script(UnicodeScript::cans,"cans"),simple_script(UnicodeScript::nkoo,"nko "),
    simple_script(UnicodeScript::olck,"olck"),simple_script(UnicodeScript::lisu,"lisu"),simple_script(UnicodeScript::orkh,"orkh"),
    simple_script(UnicodeScript::ogam,"ogam"),simple_script(UnicodeScript::glag,"glag"),simple_script(UnicodeScript::copt,"copt"),
    simple_script(UnicodeScript::lyci,"lyci"),simple_script(UnicodeScript::cari,"cari"),simple_script(UnicodeScript::ital,"ital"),
    simple_script(UnicodeScript::goth,"goth"),simple_script(UnicodeScript::ugar,"ugar"),simple_script(UnicodeScript::xpeo,"xpeo"),
    simple_script(UnicodeScript::shaw,"shaw"),simple_script(UnicodeScript::xsux,"xsux"),simple_script(UnicodeScript::cprt,"cprt"),
    simple_script(UnicodeScript::armi,"armi"),simple_script(UnicodeScript::phnx,"phnx"),simple_script(UnicodeScript::lydi,"lydi"),
    simple_script(UnicodeScript::merc,"merc"),simple_script(UnicodeScript::mero,"mero"),simple_script(UnicodeScript::sarb,"sarb"),
    simple_script(UnicodeScript::narb,"narb"),simple_script(UnicodeScript::prti,"prti"),simple_script(UnicodeScript::phli,"phli"),
    simple_script(UnicodeScript::avst,"avst"),simple_script(UnicodeScript::hatr,"hatr"),simple_script(UnicodeScript::nbat,"nbat"),
    simple_script(UnicodeScript::palm,"palm"),simple_script(UnicodeScript::hung,"hung"),simple_script(UnicodeScript::dsrt,"dsrt"),
    simple_script(UnicodeScript::osma,"osma"),simple_script(UnicodeScript::osge,"osge"),simple_script(UnicodeScript::linb,"linb"),
    simple_script(UnicodeScript::lina,"lina"),simple_script(UnicodeScript::cpmn,"cpmn"),simple_script(UnicodeScript::aghb,"aghb"),
    simple_script(UnicodeScript::elba,"elba"),simple_script(UnicodeScript::perm,"perm"),simple_script(UnicodeScript::vith,"vith"),
    simple_script(UnicodeScript::hluw,"hluw"),simple_script(UnicodeScript::mroo,"mroo"),simple_script(UnicodeScript::pauc,"pauc"),
    simple_script(UnicodeScript::tale,"tale"),simple_script(UnicodeScript::sora,"sora"),simple_script(UnicodeScript::bopo,"bopo"),
    simple_script(UnicodeScript::nshu,"nshu"),simple_script(UnicodeScript::tnsa,"tnsa"),simple_script(UnicodeScript::wara,"wara"),
    simple_script(UnicodeScript::mend,"mend"),simple_script(UnicodeScript::bass,"bass"),
};
inline constexpr u32 simple_script_count=sizeof(simple_scripts)/sizeof(simple_scripts[0]);
// Tags alone, so runtime lookups do not keep the script column.
struct SimpleScriptTags {u32 values[simple_script_count];};
inline constexpr SimpleScriptTags simple_script_tags=[]() consteval noexcept {
    SimpleScriptTags result{};for(u32 i=0;i<simple_script_count;++i)result.values[i]=simple_scripts[i].tag;return result;
}();
}
