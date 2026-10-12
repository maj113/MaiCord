#include "maicord/text/line_break.hpp"

namespace maicord {namespace {
using B=UnicodeLineBreak;using G=UnicodeGeneralCategory;
constexpr B none=static_cast<B>(255);
constexpr u8 tag_east=0x40,ignored=0x80,quote_initial=49,quote_final=50,future_ideograph=51,future_alphabetic=52;
constexpr u8 east=1,initial=2,final=4,dotted=8,potential_emoji=16,initial_context=32,numeric_close=64;
struct Item {B kind;u8 flags;};
// The pinned table has only ID/XX for Cn & Extended_Pictographic (including the
// derived property's nontrivial @missing ranges, not just explicit rows).
// The exhaustive property-contract test checks this before a data update can
// silently invalidate the compact tag. Spare class codes retain QU Pi/Pf and
// future emoji; bit 6 retains East Asian width. No second property lookup.
static_assert(static_cast<u8>(B::zwj)==48&&unicode_data_version==0x00110000);
inline B type(u8 tag) noexcept {const u8 value=tag&63;return value<49?static_cast<B>(value):value==future_ideograph?B::id:value==future_alphabetic?B::al:B::qu;}
inline bool hard(B b) noexcept {return b==B::bk||b==B::cr||b==B::lf||b==B::nl;}
inline bool letter(B b) noexcept {return b==B::al||b==B::hl;}
inline bool hangul(B b) noexcept {return b==B::jl||b==B::jv||b==B::jt||b==B::h2||b==B::h3;}
inline bool ideograph(B b) noexcept {return b==B::id||b==B::eb||b==B::em;}
inline bool affix(B b) noexcept {return b==B::pr||b==B::po;}
inline bool aksara(Item b) noexcept {return b.kind==B::ak||b.kind==B::as||(b.flags&dotted);}
bool quote_start(B b) noexcept {return b==none||hard(b)||b==B::op||b==B::qu||b==B::gl||b==B::sp||b==B::zw;}
bool quote_end(B b) noexcept {return b==none||hard(b)||b==B::sp||b==B::gl||b==B::wj||b==B::cl||b==B::qu||b==B::cp||b==B::ex||b==B::is||b==B::sy||b==B::zw;}
inline Item describe(u32 scalar,u8 tag) noexcept {
    Item out{type(tag),0};const u8 value=tag&63;
    if(tag&tag_east)out.flags|=east;
    if(value==quote_initial)out.flags|=initial;if(value==quote_final)out.flags|=final;
    if(scalar==0x25cc)out.flags|=dotted;
    if(value==future_ideograph||value==future_alphabetic)out.flags|=potential_emoji;
    return out;
}
// Called only for significant punctuation/syllable positions. Each call skips
// a disjoint ignored span; two-token lookahead gives a constant revisit bound.
u32 following(Slice<u8> tags,u32 count,u32 at) noexcept {
    if(at<count)++at;while(at<count&&(tags[at]&ignored))++at;return at;
}
}
bool unicode_line_breaks(ConstSlice<u32> text,Slice<u8> out) noexcept {
    if((text.count&&!text.data)||!out.data||out.count<=text.count)return false;
    for(u32 i=0;i<text.count;++i)if(!unicode_is_scalar(text[i]))return false;
    if(!text.count){out[0]=unicode_line_prohibited;return true;}
    u8 base=255;
    for(u32 i=0;i<text.count;++i){
        const auto& p=*unicode_properties(text[i]);B b=p.line_break();u8 tag=0;
        if(b==B::ai||b==B::sg||b==B::xx)b=B::al; // LB1
        else if(b==B::sa)b=p.general_category()==G::mn||p.general_category()==G::mc?B::cm:B::al;
        else if(b==B::cj)b=B::ns;
        if(b==B::cm||b==B::zwj){
            const B previous=base==255?none:type(base);
            if(previous!=none&&!hard(previous)&&previous!=B::sp&&previous!=B::zw)tag=base|ignored; // LB9
            else tag=static_cast<u8>(B::al); // LB10: all ancillary properties become those of A.
        }else{
            tag=static_cast<u8>(b);const auto g=p.general_category();
            if(b==B::qu&&g==G::pi)tag=quote_initial;else if(b==B::qu&&g==G::pf)tag=quote_final;
            else if(g==G::cn&&p.has(UnicodeFlag::extended_pictographic))tag=b==B::id?future_ideograph:future_alphabetic;
            if(p.has(UnicodeFlag::east_asian_line_break))tag|=tag_east;
        }
        out[i]=tag;if(!(tag&ignored))base=tag;
    }
    Item previous{none,0},before{none,0},nonspace{none,0};bool raw_zwj=false,numeric=false,ri_odd=false;
    for(u32 i=0;i<text.count;++i){
        const u8 tag=out[i];const B b=type(tag),a=previous.kind;u8 split=unicode_line_allowed;
        Item current{b,0};if(!(tag&ignored))current=describe(text[i],tag);
        if(!i)split=unicode_line_prohibited; // LB2
        else if(a==B::bk)split=unicode_line_mandatory; // LB4
        else if(a==B::cr&&b==B::lf)split=unicode_line_prohibited; // LB5
        else if(hard(a))split=unicode_line_mandatory;
        else if(hard(b)||b==B::sp||b==B::zw)split=unicode_line_prohibited; // LB6/7
        else if(nonspace.kind==B::zw)split=unicode_line_allowed; // LB8
        else if(raw_zwj||(tag&ignored))split=unicode_line_prohibited; // LB8a/9
        else {
            // Lookahead is evaluated at most once per significant current
            // position, and at most twice for OP IS NU. Ignored characters do
            // not themselves scan, so a long mark suffix cannot cause O(n^2).
            Item next{none,0};B next2=none;
            if(b==B::qu||(a==B::sp&&b==B::is)||(affix(a)&&b==B::op)||(aksara(previous)&&aksara(current))){
                const u32 at=following(out,text.count,i);
                if(at<text.count){next=describe(text[at],out[at]);
                    if(affix(a)&&b==B::op&&next.kind==B::is){const u32 at2=following(out,text.count,at);if(at2<text.count)next2=type(out[at2]);}}
            }
            if(a==B::wj||b==B::wj||a==B::gl)split=0; // LB11/12
            else if(b==B::gl&&a!=B::sp&&a!=B::ba&&a!=B::hy&&a!=B::hh)split=0; // LB12a
            else if(b==B::cl||b==B::cp||b==B::ex||b==B::sy)split=0; // LB13
            else if(nonspace.kind==B::op)split=0; // LB14
            else if(nonspace.kind==B::qu&&(nonspace.flags&initial_context))split=0; // LB15a
            else if(b==B::qu&&(current.flags&final)&&quote_end(next.kind))split=0; // LB15b
            else if(a==B::sp&&b==B::is&&next.kind==B::nu)split=1; // LB15c
            else if(b==B::is)split=0; // LB15d
            else if((nonspace.kind==B::cl||nonspace.kind==B::cp)&&b==B::ns)split=0; // LB16
            else if(nonspace.kind==B::b2&&b==B::b2)split=0; // LB17
            else if(a==B::sp)split=1; // LB18
            else if((b==B::qu&&!(current.flags&initial))||(a==B::qu&&!(previous.flags&final)))split=0; // LB19
            else if((b==B::qu&&(!(previous.flags&east)||!(next.flags&east)))||
                    (a==B::qu&&(!(current.flags&east)||!(before.flags&east))))split=0; // LB19a
            else if(a==B::cb||b==B::cb)split=1; // LB20
            else if((a==B::hy||a==B::hh)&&letter(b)&&(before.kind==none||hard(before.kind)||before.kind==B::sp||before.kind==B::zw||before.kind==B::cb||before.kind==B::gl))split=0; // LB20a
            else if(b==B::ba||b==B::hh||b==B::hy||b==B::ns||a==B::bb)split=0; // LB21
            else if(before.kind==B::hl&&(a==B::hy||a==B::hh)&&b!=B::hl)split=0; // LB21a
            else if(a==B::sy&&b==B::hl)split=0; // LB21b
            else if(b==B::in)split=0; // LB22
            else if((letter(a)&&b==B::nu)||(a==B::nu&&letter(b)))split=0; // LB23
            else if((a==B::pr&&ideograph(b))||(ideograph(a)&&b==B::po))split=0; // LB23a
            else if((affix(a)&&letter(b))||(letter(a)&&affix(b)))split=0; // LB24
            else if((affix(b)&&(numeric||(previous.flags&numeric_close)))||
                    (affix(a)&&b==B::op&&(next.kind==B::nu||(next.kind==B::is&&next2==B::nu)))||
                    (b==B::nu&&(affix(a)||a==B::hy||a==B::is||numeric)))split=0; // LB25
            else if((a==B::jl&&(b==B::jl||b==B::jv||b==B::h2||b==B::h3))||
                    ((a==B::jv||a==B::h2)&&(b==B::jv||b==B::jt))||((a==B::jt||a==B::h3)&&b==B::jt))split=0; // LB26
            else if((hangul(a)&&b==B::po)||(a==B::pr&&hangul(b)))split=0; // LB27
            else if(letter(a)&&letter(b))split=0; // LB28
            else if((a==B::ap&&aksara(current))||(aksara(previous)&&(b==B::vf||b==B::vi))||
                    (aksara(before)&&a==B::vi&&(b==B::ak||(current.flags&dotted)))||
                    (aksara(previous)&&aksara(current)&&next.kind==B::vf))split=0; // LB28a
            else if(a==B::is&&letter(b))split=0; // LB29
            else if(((letter(a)||a==B::nu)&&b==B::op&&!(current.flags&east))||
                    (a==B::cp&&!(previous.flags&east)&&(letter(b)||b==B::nu)))split=0; // LB30
            else if(a==B::ri&&b==B::ri&&ri_odd)split=0; // LB30a
            else if((a==B::eb||(previous.flags&potential_emoji))&&b==B::em)split=0; // LB30b
        }
        out[i]=split;raw_zwj=text[i]==0x200d;
        if(!(tag&ignored)){
            if(b==B::qu&&(current.flags&initial)&&quote_start(a))current.flags|=initial_context;
            if((b==B::cl||b==B::cp)&&numeric)current.flags|=numeric_close;
            numeric=b==B::nu||((b==B::sy||b==B::is)&&numeric);
            ri_odd=b==B::ri?!ri_odd:false;
            before=previous;previous=current;if(b!=B::sp)nonspace=current;
        }
    }
    out[text.count]=unicode_line_mandatory;return true; // LB3
}
}
