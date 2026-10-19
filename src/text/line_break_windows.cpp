#include "maicord/text/detail/line_break_windows.hpp"
// Windows-compatible en-US line-break policy; the default Unicode implementation
// is separate.
namespace maicord {namespace {
using B=UnicodeLineBreak;
struct LineClassOverride {u32 first;u16 count;B kind;};
static_assert(sizeof(LineClassOverride)==8);
#include "generated/line_windows_data.inc"
inline B compatibility_class(u32 cp,B current) noexcept {
    constexpr u32 count=sizeof(line_class_overrides)/sizeof(line_class_overrides[0]);
    if(cp<line_class_overrides[0].first||cp>=line_class_overrides[count-1].first+line_class_overrides[count-1].count)return current;
    u32 first=0,last=count;while(first<last){const u32 middle=first+(last-first)/2;if(line_class_overrides[middle].first<=cp)first=middle+1;else last=middle;}
    if(first){const auto& r=line_class_overrides[first-1];if(cp-r.first<r.count)return r.kind;}return current;
}
constexpr B none=static_cast<B>(255);constexpr u8 ignored=128,selector=64;
inline B type(u8 tag) noexcept {return static_cast<B>(tag&63);}
inline bool hard(B b) noexcept {return b==B::bk||b==B::cr||b==B::lf||b==B::nl;}
inline bool letter(B b) noexcept {return b==B::al||b==B::hl;}
inline bool affix(B b) noexcept {return b==B::pr||b==B::po;}
inline bool hangul(B b) noexcept {return b==B::jl||b==B::jv||b==B::jt||b==B::h2||b==B::h3;}
inline bool ideograph(B b) noexcept {return b==B::id||b==B::eb||b==B::em;}
}
bool line_windows_candidate(ConstSlice<u32> text,Slice<u8> out) noexcept {
    if((text.count&&!text.data)||!out.data||out.count<=text.count)return false;
    for(u32 i=0;i<text.count;++i)if(!unicode_is_scalar(text[i]))return false;
    // One pass: every rule looks only backward, so each class is resolved and
    // its boundary decided before the next scalar is read.
    u8 base=255;bool previous_selector=false;B previous=none,before=none,nonspace=none;
    for(u32 i=0;i<text.count;++i){const auto& p=*unicode_properties(text[i]);B b=compatibility_class(text[i],p.line_break());u8 tag=0;
        // CGJ and ZWJ retain glue behavior across following combining marks.
        // Emoji modifiers instead inherit the preceding base through CM.
        if(text[i]==0x34f)b=B::gl;
        else if(b==B::ai||b==B::sg||b==B::xx||b==B::sa||b==B::ak||b==B::as||b==B::ap)b=B::al;
        else if(b==B::cj)b=B::ns;else if(b==B::hh)b=B::ba;else if(b==B::vf||b==B::vi||b==B::em)b=B::cm;
        else if(b==B::zwj)b=B::gl;
        // One selector extends its preceding character, including a newline.
        // A selector cannot itself be the base of another selector sequence.
        const bool is_selector=p.has(UnicodeFlag::variation_selector);
        if(is_selector&&!previous_selector&&base!=255)tag=base|ignored|selector;
        else if(b==B::cm){if(previous!=none&&!hard(previous)&&previous!=B::sp&&previous!=B::zw)tag=base|ignored;else tag=static_cast<u8>(B::al);
        }else tag=static_cast<u8>(b);
        previous_selector=is_selector;b=type(tag);const B a=previous;u8 split=1;
        if(!i)split=0;
        // Shortcuts for the common word pairs; the chain below gives the
        // same results (LB28, and LB14 then LB18).
        else if(!(tag&ignored)&&letter(b)&&(letter(a)||a==B::sp))split=letter(a)||nonspace==B::op?0:1;
        else if(tag&selector)split=0;
        else if(a==B::bk)split=2;
        else if(a==B::cr&&b==B::lf)split=0;
        else if(hard(a))split=2;
        else if(hard(b)||b==B::sp||b==B::zw)split=0;
        else if(nonspace==B::zw)split=1;
        else if(tag&ignored)split=0;
        else if(a==B::wj||b==B::wj||a==B::gl)split=0;
        else if(b==B::gl&&a!=B::sp&&a!=B::ba&&a!=B::hy)split=0;
        else if(b==B::cl||b==B::cp||b==B::ex||b==B::is||b==B::sy)split=0;
        else if(nonspace==B::op||(nonspace==B::qu&&b==B::op))split=0;
        else if((nonspace==B::cl||nonspace==B::cp)&&b==B::ns)split=0;
        else if(nonspace==B::b2&&b==B::b2)split=0;
        else if(a==B::sp)split=1;
        else if(a==B::qu||b==B::qu)split=0;
        else if(a==B::cb||b==B::cb)split=1;
        else if(b==B::ba||b==B::hy||b==B::ns||a==B::bb)split=0;
        else if(before==B::hl&&(a==B::hy||a==B::ba))split=0;
        else if(b==B::in&&(letter(a)||ideograph(a)||a==B::in||a==B::nu||hangul(a)))split=0;
        else if((letter(a)&&b==B::nu)||(a==B::nu&&letter(b)))split=0;
        else if((a==B::pr&&ideograph(b))||(ideograph(a)&&b==B::po))split=0;
        else if(affix(a)&&letter(b))split=0;
        else if(((a==B::cl||a==B::cp||a==B::nu)&&affix(b))||(affix(a)&&(b==B::op||b==B::nu))||
                ((a==B::hy||a==B::is||a==B::sy||a==B::nu)&&b==B::nu))split=0;
        else if((a==B::jl&&b==B::jv)||(a==B::jv&&b==B::jt))split=0;
        else if((hangul(a)&&b==B::po)||(a==B::pr&&hangul(b)))split=0;
        else if(letter(a)&&letter(b))split=0;
        else if(a==B::is&&letter(b))split=0;
        else if(((letter(a)||a==B::nu)&&b==B::op)||(a==B::cp&&(letter(b)||b==B::nu)))split=0;
        else if(a==B::ri&&b==B::ri)split=0;
        out[i]=split;
        if(!(tag&ignored)){base=tag;before=previous;previous=b;if(b!=B::sp)nonspace=b;}
    }
    if(!text.count){out[0]=0;return true;}
    out[text.count]=hard(previous)?2:1;return true;
}
}
