#include "maicord/text/unicode.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
bool unicode_grapheme_tests() noexcept;
namespace {
bool cases() noexcept {
    // Validate the pinned-data lower bounds used by Latin's cheap exclusions.
    for(u32 cp=0;cp<0x300;++cp)if(unicode_properties(cp)->combining_class())return false;
    for(u32 cp=0;cp<0xad;++cp){const auto* p=unicode_properties(cp);if(p->has(UnicodeFlag::default_ignorable_code_point)||p->has(UnicodeFlag::variation_selector))return false;}
    const auto* latin=unicode_properties('A');const auto* mark=unicode_properties(0x301);
    if(!latin || !mark || latin->bidi_class()!=UnicodeBidiClass::l || latin->general_category()!=UnicodeGeneralCategory::lu ||
       latin->script()!=UnicodeScript::latn || mark->combining_class()!=230)return false;
    if(unicode_properties(0x628)->joining_type()!=UnicodeJoiningType::d || unicode_properties(0x628)->bidi_class()!=UnicodeBidiClass::al ||
       unicode_properties(0x5d0)->bidi_class()!=UnicodeBidiClass::r || unicode_properties(0x5ff)->bidi_class()!=UnicodeBidiClass::r ||
       unicode_properties(0x1ecbf)->bidi_class()!=UnicodeBidiClass::al || unicode_properties(0x2066)->bidi_class()!=UnicodeBidiClass::lri)return false;
    if(!unicode_properties(0x1f469)->has(UnicodeFlag::extended_pictographic) || !unicode_properties(0x1f3fb)->has(UnicodeFlag::emoji) ||
       unicode_properties(0x1f1e6)->grapheme_break()!=UnicodeGraphemeBreak::ri || unicode_properties(0x94d)->indic_conjunct_break()!=UnicodeIndicConjunctBreak::linker)return false;
    if(!unicode_properties(0x628)->has(UnicodeFlag::arabic_script_extension) ||
       !unicode_properties(0x640)->has(UnicodeFlag::arabic_script_extension) ||
       latin->has(UnicodeFlag::arabic_script_extension) || unicode_script_tag(UnicodeScript::latn)!=0x4c61746e)return false;
    if(!unicode_properties(0x30fc)->has(UnicodeFlag::east_asian_line_break) ||
       !unicode_properties(0xff66)->has(UnicodeFlag::east_asian_line_break) ||
       !unicode_properties(0x3000)->has(UnicodeFlag::east_asian_line_break) ||
       unicode_properties(0xa1)->has(UnicodeFlag::east_asian_line_break) || latin->has(UnicodeFlag::east_asian_line_break))return false;
    if(unicode_properties(0xd800)->general_category()!=UnicodeGeneralCategory::cs || unicode_is_scalar(0xd800) || unicode_is_scalar(0xdfff) ||
       !unicode_is_scalar(0x10ffff) || unicode_properties(0x10ffff)->general_category()!=UnicodeGeneralCategory::cn ||
       unicode_properties(0x110000) || unicode_properties(~0u) || unicode_is_scalar(~0u))return false;
    const auto bracket=unicode_bidi_bracket('(');
    return unicode_bidi_mirror('(')==')' && unicode_bidi_mirror(0x2208)==0x220b && bracket.paired==')' && bracket.type==UnicodeBracketType::open &&
           unicode_bidi_bracket('A').type==UnicodeBracketType::none && unicode_bidi_mirror('A')=='A' && unicode_property_data_bytes()==51922;
}
u8 output_buffer[16384];u32 output_used=0;
bool flush(HANDLE file) noexcept {
    u32 at=0;
    while(at<output_used){DWORD written=0;if(!WriteFile(file,output_buffer+at,output_used-at,&written,nullptr) || !written)return false;at+=written;}
    output_used=0;return true;
}
bool append(HANDLE file,const void* bytes,u32 count) noexcept {
    if(count>sizeof(output_buffer))return false;
    if(output_used+count>sizeof(output_buffer) && !flush(file))return false;
    copy_bytes(output_buffer+output_used,bytes,count);output_used+=count;return true;
}
bool dump(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    bool ok=append(file,"MCUCDS2",8);
    for(u32 cp=0;cp<0x110000 && ok;++cp) {
        const auto& p=*unicode_properties(cp);const auto bracket=unicode_bidi_bracket(cp);
        const u8 fields[]{static_cast<u8>(p.bidi_class()),static_cast<u8>(p.general_category()),static_cast<u8>(p.script()),p.combining_class(),
            static_cast<u8>(p.grapheme_break()),static_cast<u8>(p.word_break()),static_cast<u8>(p.line_break()),static_cast<u8>(p.joining_type()),
            static_cast<u8>(p.indic_conjunct_break()),static_cast<u8>(p.indic_syllabic_category()),
            static_cast<u8>(p.indic_positional_category())};
        const u32 numbers[]{p.flags(),unicode_bidi_mirror(cp),bracket.paired};
        const u8 suffix[]{static_cast<u8>(bracket.type)};
        ok=append(file,fields,sizeof(fields)) && append(file,numbers,sizeof(numbers)) && append(file,suffix,sizeof(suffix));
    }
    ok=flush(file) && ok;CloseHandle(file);return ok;
}
int run() noexcept {
    if(!cases() || !unicode_grapheme_tests())return 1;
    // Optional QA dump path. Quoted paths are supported; extra arguments fail.
    const wchar_t* command=GetCommandLineW();bool quote=false;
    while(*command){if(*command=='"')quote=!quote;else if(!quote && (*command==' ' || *command=='\t'))break;++command;}
    while(*command==' ' || *command=='\t')++command;
    if(*command) {
        wchar_t path[32768];u32 count=0;quote=*command=='"';if(quote)++command;
        while(*command && !(quote?*command=='"':(*command==' ' || *command=='\t'))) {
            if(count+1>=32768)return 2;path[count++]=*command++;
        }
        if(quote){if(*command!='"')return 2;++command;}
        while(*command==' ' || *command=='\t')++command;
        if(*command || !count)return 2;path[count]=0;if(!dump(path))return 3;
    }
    constexpr auto message=utf8_literal("PASS Unicode properties and graphemes\n");
    DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message.data,message.bytes,&written,nullptr);
    return 0;
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_unicode_tests_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
