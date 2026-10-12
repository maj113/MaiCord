#pragma once
#include "maicord/text/unicode.hpp"
namespace maicord {
// One byte selects the packed profile. Keep its full eight-bit identity space;
// Unassigned scalars use Common shaping and the ordinary missing-glyph route.
// Only an unresolved Inherited item still requires a preceding script owner.
// Indexing covers the entire enum representation, including unsupported values.
struct NativeTextScriptProfiles {u8 values[256];};
inline constexpr NativeTextScriptProfiles native_text_script_profiles=[]() constexpr noexcept {
    NativeTextScriptProfiles result{};
    const auto both=[&](UnicodeScript script,u8 profile) constexpr noexcept {
        result.values[static_cast<u8>(script)]=profile;
    };
    both(UnicodeScript::latn,1);both(UnicodeScript::grek,2);both(UnicodeScript::cyrl,4);
    both(UnicodeScript::arab,6);both(UnicodeScript::hebr,8);both(UnicodeScript::hani,10);
    // Hiragana and Katakana share a kana font program, but retain distinct
    // shaping/layout groups so float accumulation and caret origins agree.
    both(UnicodeScript::hira,15);both(UnicodeScript::kana,12);both(UnicodeScript::hang,14);
    both(UnicodeScript::zyyy,16);both(UnicodeScript::armn,20);both(UnicodeScript::geor,22);
    both(UnicodeScript::thai,24);both(UnicodeScript::laoo,26);both(UnicodeScript::deva,28);
    both(UnicodeScript::ethi,30);both(UnicodeScript::cher,32);both(UnicodeScript::taml,34);
    both(UnicodeScript::beng,36);both(UnicodeScript::gujr,38);both(UnicodeScript::guru,40);
    both(UnicodeScript::orya,42);both(UnicodeScript::telu,44);both(UnicodeScript::knda,46);both(UnicodeScript::mlym,48);both(UnicodeScript::sinh,50);both(UnicodeScript::khmr,52);both(UnicodeScript::mymr,54);both(UnicodeScript::tibt,56);
    both(UnicodeScript::egyp,58);both(UnicodeScript::runr,60);
    // Simple scripts without reordering or required contextual forms: nominal
    // cmap glyphs from the first covering face (Ebrima, Gadugi, Microsoft Yi
    // Baiti, Segoe UI Historic, Nirmala UI ...), like Runic and Cherokee. N'Ko
    // is right-to-left and draws its nominal (unjoined) forms.
    both(UnicodeScript::bamu,64);both(UnicodeScript::yiii,66);both(UnicodeScript::vaii,68);
    both(UnicodeScript::tfng,70);both(UnicodeScript::cans,72);both(UnicodeScript::nkoo,74);
    both(UnicodeScript::olck,76);both(UnicodeScript::lisu,78);
    // Braille cells are independent symbols. Use the Common font program
    // with ordinary Unicode coverage selection, including the blank cell.
    both(UnicodeScript::brai,16);
    result.values[static_cast<u8>(UnicodeScript::zinh)]=16;
    result.values[static_cast<u8>(UnicodeScript::zzzz)]=16;
    return result;
}();
constexpr u8 native_text_script_profile(UnicodeScript script,bool resolved) noexcept {
    if(resolved&&script==UnicodeScript::zinh)return 0;
    return native_text_script_profiles.values[static_cast<u8>(script)];
}
}
