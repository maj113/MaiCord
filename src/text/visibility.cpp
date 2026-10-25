#include "maicord/text/unicode.hpp"
namespace maicord {
bool native_text_nonvisual(u32 cp) noexcept {
    switch(cp>>16){
        case 0:
            if(cp<0x100)return cp<32||cp-0x7fu<33||cp==0xad;
            if(cp-0x2000u<128){
                constexpr u64 low=(0x7full<<40)|(1ull<<11)|(3ull<<14);
                constexpr u64 high=(0x1full<<32)|(0x3ffull<<38);
                return (((cp&64)?high:low)>>(cp&63)&1)!=0;}
            return cp==0xfeff||cp-0xfff9u<3||cp>=0xfffe;
        case 1:return cp-0x1bca0u<4||cp-0x1d173u<8;
        case 14:return cp==0xe0001||cp-0xe0020u<96;
        default:return false;
    }
}
namespace {
bool g_browser_invisibles=false;
bool filler(u32 cp) noexcept {return cp==0x115f||cp==0x1160;}
// A UTF-8 lead byte that can start a scalar text_hide_default_ignorables hides
// (or replaces); text without one is returned unchanged without decoding.
constexpr bool candidate_lead(u8 b) noexcept {return b==0xc2||b==0xd8||b==0xe1||b==0xe2||b==0xe3||b==0xef||b==0xf0||b==0xf3;}
// A conjoining jamo other than the fillers; fillers next to one (possibly via
// other fillers) belong to the syllable the shaper composes.
bool conjoining_jamo(u32 cp) noexcept {return (cp-0x1100u<0x100||cp-0xa960u<0x20||cp-0xd7b0u<0x50)&&!filler(cp);}
// A variation selector stays only after a base that can consume it; any other
// selector is an orphan the font has no sequence for.
bool selector_consumed(u32 previous,u32 cp) noexcept {
    if(!previous||!unicode_is_scalar(previous))return false;
    const auto* p=unicode_properties(previous);
    if(p->has(UnicodeFlag::variation_selector)||p->has(UnicodeFlag::default_ignorable_code_point)||p->has(UnicodeFlag::white_space))return false;
    if(cp==0xfe0e||cp==0xfe0f)return p->has(UnicodeFlag::emoji);
    const auto script=p->script();
    if(cp-0x180bu<5)return script==UnicodeScript::mong;
    return script==UnicodeScript::hani||script==UnicodeScript::mong||script==UnicodeScript::mymr||script==UnicodeScript::phag||
        p->general_category()==UnicodeGeneralCategory::sm;
}
bool hidden(u32 previous,u32 cp,u32 next) noexcept {
    if(filler(cp))return !conjoining_jamo(previous)&&!conjoining_jamo(next);
    if(cp==0x17b4||cp==0x17b5)return !(previous-0x1780u<0x80&&previous!=0x17b4&&previous!=0x17b5);
    if(cp==0x2065||cp==0x3164||cp==0xffa0||cp-0xfff0u<9)return true;
    // Invisible format controls the native shaper has no item for: C1 controls,
    // soft hyphen, ALM, LRM/RLM, word joiner and invisible operators, deprecated
    // format controls, BOM, interlinear annotation, shorthand and musical format
    // controls.
    if((cp-0x80u<32&&cp!=0x85)||cp==0xad||cp==0x61c||cp-0x200eu<2||cp-0x2060u<5||cp-0x206au<6||cp==0xfeff||
       cp-0xfff9u<3||cp-0x1bca0u<4||cp-0x1d173u<8)return true;
    if(cp-0x180bu<5||cp-0xfe00u<16||cp-0xe0100u<240)return !selector_consumed(previous,cp);
    return cp==0xe0000||cp-0xe0002u<30||cp-0xe0080u<128||cp-0xe01f0u<0xe10;
}
}
void text_set_browser_invisibles(bool enabled) noexcept {g_browser_invisibles=enabled;}
bool text_browser_invisibles() noexcept {return g_browser_invisibles;}
u32 text_hide_default_ignorables(Utf8View input,Slice<char> output) noexcept {
    if(!input.data||!input.bytes)return 0;
    bool candidate=false;
    for(u32 i=0;i<input.bytes&&!candidate;++i)candidate=candidate_lead(static_cast<u8>(input.data[i]));
    if(!candidate)return 0;
    u32 previous=0,solid=0,changed=0;bool tagged=false;
    for(u32 at=0;at<input.bytes;){const auto cp=utf8_decode_one(input.data+at,input.bytes-at);if(!cp.valid||!cp.width)return changed?~0u:0;
        // Fillers look past adjacent fillers to the jamo they join.
        const bool joins=filler(cp.codepoint);u32 next=0;
        for(u32 ahead=at+cp.width;ahead<input.bytes;){const auto n=utf8_decode_one(input.data+ahead,input.bytes-ahead);if(!n.valid||!n.width)break;
            next=n.codepoint;if(!joins||!filler(next))break;ahead+=n.width;}
        // Tag characters only continue an emoji tag sequence begun by U+1F3F4
        // (subdivision flags); any other tag, and the deprecated language tag
        // U+E0001, has no glyph sequence and would draw as .notdef.
        const bool tag=cp.codepoint==0xe0001||cp.codepoint-0xe0020u<96;
        const bool hide=tag?cp.codepoint==0xe0001||!tagged:hidden(joins?solid:previous,cp.codepoint,next);
        tagged=cp.codepoint==0x1f3f4||(tagged&&cp.codepoint-0xe0020u<95);
        if(cp.width>=2&&hide){
            if(!changed){if(output.count<input.bytes||!output.data)return ~0u;for(u32 i=0;i<at;++i)output[i]=input.data[i];}
            // U+034F COMBINING GRAPHEME JOINER (one, or two for a supplementary
            // scalar) or U+200C (zero width, no break before it like a selector):
            // invisible in every shaping path. Tag characters such as U+E0001
            // take an advance in the native shaper.
            const char* replacement=cp.width==3?"\xe2\x80\x8c":"\xcd\x8f\xcd\x8f";for(u32 i=0;i<cp.width;++i)output[at+i]=replacement[i];++changed;
        }else if(changed){for(u32 i=0;i<cp.width;++i)output[at+i]=input.data[at+i];}
        previous=cp.codepoint;if(!filler(previous))solid=previous;at+=cp.width;
    }
    return changed?input.bytes:0;
}
}
