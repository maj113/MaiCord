#include "maicord/text/unicode/detail/line_break.hpp"
namespace maicord {namespace {
using B=UnicodeLineBreak;
// The class before the first scalar; it is in a set only where a rule names sot.
constexpr B sot=static_cast<B>(static_cast<u32>(B::zwj)+1);
template<class... T> constexpr u64 set(T... b) noexcept {return ((u64{1}<<static_cast<u32>(b))|...);}
constexpr bool in(u64 s,B b) noexcept {return (s>>static_cast<u32>(b))&1;}
constexpr u64 hard=set(B::bk,B::cr,B::lf,B::nl),letter=set(B::al,B::hl),affix=set(B::pr,B::po),
    hangul=set(B::jl,B::jv,B::jt,B::h2,B::h3),ideograph=set(B::id,B::eb,B::em),
    // LB1 resolves these before any rule.
    resolved=set(B::ai,B::sg,B::xx,B::sa,B::cj),
    // LB15b: what may follow a final quotation mark that keeps the space before it.
    after_final=set(B::sp,B::gl,B::wj,B::cl,B::qu,B::cp,B::ex,B::is,B::sy,B::bk,B::cr,B::lf,B::nl,B::zw);
// Properties of a base the rules read besides its class: $EastAsian, Pi or Pf
// quotation marks, the LB28a syllable bases (AK, AS, U+25CC), U+25CC itself and
// [\p{Extended_Pictographic}&\p{Cn}].
constexpr u8 wide=1,initial=2,final=4,aksara=8,dotted=16,unassigned_pictograph=32;
struct Base {B type;u8 flags;};
// A boundary whose rule reads the bases after it keeps its fallback until they
// arrive: LB15b, LB15c, LB19a, LB25 (two bases) and LB28a.
enum class Defer : u8 {none,final_quote,decimal,quote,prefix,prefix_decimal,syllable};
static_assert(static_cast<u32>(Defer::syllable)<8); // the scan keeps it in three bits
// What the rules read besides the pair: the base before a, the last class
// other than SP (nonspace), LB14/LB15a (glue), LB8a (joiner), the LB25
// number state (1: NU (SY|IS)*, 2: then CL or CP) and LB30a parity.
struct Context {Base a,before;B nonspace;bool glue,joiner,ri_odd;u8 number;};
struct Verdict {u8 split;Defer next;};
// LB2 to LB31 for the boundary between a (in the context) and b.
constexpr Verdict decide(const Context& c,B b,u8 flags) noexcept {
    const B a=c.a.type;const u8 af=c.a.flags;
    if(a==sot)return {0,Defer::none}; // LB2
    if(a==B::cr&&b==B::lf)return {0,Defer::none}; // LB5
    if(in(hard,a))return {2,Defer::none}; // LB4, LB5
    if(in(hard|set(B::sp,B::zw),b))return {0,Defer::none}; // LB6, LB7
    if(c.nonspace==B::zw)return {1,Defer::none}; // LB8
    if(c.joiner)return {0,Defer::none}; // LB8a
    if(b==B::wj||a==B::wj||a==B::gl)return {0,Defer::none}; // LB11, LB12
    if(b==B::gl&&!in(set(B::sp,B::ba,B::hy,B::hh),a))return {0,Defer::none}; // LB12a
    if(in(set(B::cl,B::cp,B::ex,B::sy),b))return {0,Defer::none}; // LB13
    if(c.glue)return {0,Defer::none}; // LB14, LB15a
    if(a==B::sp&&(flags&final))return {1,Defer::final_quote}; // LB15b, else LB18
    if(b==B::is)return {0,a==B::sp?Defer::decimal:Defer::none}; // LB15c, LB15d
    if(b==B::ns&&(c.nonspace==B::cl||c.nonspace==B::cp))return {0,Defer::none}; // LB16
    if(b==B::b2&&c.nonspace==B::b2)return {0,Defer::none}; // LB17
    if(a==B::sp)return {1,Defer::none}; // LB18
    if((b==B::qu&&!(flags&initial))||(a==B::qu&&!(af&final)))return {0,Defer::none}; // LB19
    // LB19a: [^$EastAsian] × QU, QU × [^$EastAsian], (sot | [^$EastAsian]) QU ×
    if((b==B::qu&&!(af&wide))||(a==B::qu&&!(flags&c.before.flags&wide)))return {0,Defer::none};
    u8 split=1;Defer next=Defer::none;
    if(b==B::cb||a==B::cb)split=1; // LB20
    else if(in(set(B::hy,B::hh),a)&&in(letter,b)&&in(hard|set(B::sp,B::zw,B::cb,B::gl,sot),c.before.type))split=0; // LB20a
    else if(in(set(B::ba,B::hh,B::hy,B::ns),b)||a==B::bb)split=0; // LB21
    else if(c.before.type==B::hl&&in(set(B::hy,B::hh),a)&&b!=B::hl)split=0; // LB21a
    else if((a==B::sy&&b==B::hl)||b==B::in)split=0; // LB21b, LB22
    else if((in(letter,a)&&b==B::nu)||(a==B::nu&&in(letter,b)))split=0; // LB23
    else if((a==B::pr&&in(ideograph,b))||(in(ideograph,a)&&b==B::po))split=0; // LB23a
    else if((in(affix,a)&&in(letter,b))||(in(letter,a)&&in(affix,b)))split=0; // LB24
    else if(c.number&&in(affix,b))split=0; // LB25: NU (SY|IS)* (CL|CP)? × (PO|PR)
    else if(in(affix,a)&&b==B::op)next=Defer::prefix; // LB25: (PO|PR) × OP IS? NU
    else if(b==B::nu&&(c.number==1||in(affix|set(B::hy,B::is),a)))split=0; // LB25
    else if((a==B::jl&&in(set(B::jl,B::jv,B::h2,B::h3),b))||(in(set(B::jv,B::h2),a)&&in(set(B::jv,B::jt),b))||
            (in(set(B::jt,B::h3),a)&&b==B::jt))split=0; // LB26
    else if((in(hangul,a)&&b==B::po)||(a==B::pr&&in(hangul,b)))split=0; // LB27
    else if(in(letter,a)&&in(letter,b))split=0; // LB28
    else if((a==B::ap&&(flags&aksara))||((af&aksara)&&(b==B::vf||b==B::vi))||
            (a==B::vi&&(c.before.flags&aksara)&&(b==B::ak||(flags&dotted))))split=0; // LB28a
    else if((af&aksara)&&(flags&aksara))next=Defer::syllable; // LB28a: (AK|◌|AS) × (AK|◌|AS) VF
    else if(a==B::is&&in(letter,b))split=0; // LB29
    else if(((in(letter,a)||a==B::nu)&&b==B::op&&!(flags&wide))||(a==B::cp&&!(af&wide)&&(in(letter,b)||b==B::nu)))split=0; // LB30
    else if(a==B::ri&&b==B::ri&&c.ri_odd)split=0; // LB30a
    else if(b==B::em&&(a==B::eb||(af&unassigned_pictograph)))split=0; // LB30b
    // LB19a: × QU ([^$EastAsian] | eot) precedes LB20 to LB31.
    if(b==B::qu&&split)next=Defer::quote;
    return {split,next};
}
// Hyphens after which LB20a or LB21a reads the class before them.
constexpr u64 hyphen_context=hard|set(B::sp,B::zw,B::cb,B::gl,B::hl,sot);
// The classes after which the scan carries state beyond nonspace.
constexpr u64 stateful=set(B::nu,B::sy,B::is,B::cl,B::cp,B::ri,B::sp,B::op,B::qu,B::zw,B::hy,B::hh,B::vi);
// The pairs decide() answers from more than their two classes once no rule
// holds from further back (see held below): after sot, a hard break or QU;
// after SP before QU, IS, NS or B2 (LB15b to LB17); before QU (LB19, LB19a);
// before EM after ID or AL, the classes of [\p{Extended_Pictographic}&\p{Cn}]
// (LB30b); and the LB25, LB28a (U+25CC is AL), LB30 ($EastAsian) and LB30a
// pairs. The others take the table below.
constexpr u64 contextual(u32 index) noexcept {
    const B a=static_cast<B>(index);
    if(a==sot||in(hard|set(B::qu),a))return ~u64{0};
    if(a==B::sp)return set(B::qu,B::is,B::ns,B::b2);
    u64 s=set(B::qu);
    if(a==B::id||a==B::al)s|=set(B::em);
    if(a==B::ri)s|=set(B::ri);
    if(in(set(B::sy,B::is,B::cl,B::cp),a))s|=affix;
    if(a==B::sy)s|=set(B::nu);
    if(in(affix|letter|set(B::nu),a))s|=set(B::op);
    if(a==B::cp)s|=letter|set(B::nu);
    if(a==B::al)s|=set(B::ak,B::as,B::vf,B::vi);
    if(in(set(B::ak,B::as,B::ap),a))s|=set(B::al);
    if(in(set(B::ak,B::as),a))s|=set(B::ak,B::as);
    return s;
}
// Per class a, in the context every table pair shares (a NU is the start of a
// number; AK and AS are syllable bases): the b whose boundary decide() allows,
// and the contextual ones.
constexpr u32 rows=static_cast<u32>(sot)+1;
struct Table {u64 allowed[rows],contextual[rows];};
constexpr u8 class_flags(u32 type) noexcept {return in(set(B::ak,B::as),static_cast<B>(type))?aksara:0;}
constexpr Context neutral(u32 a) noexcept {
    return {{static_cast<B>(a),class_flags(a)},{B::al,0},static_cast<B>(a),false,false,false,static_cast<u8>(a==static_cast<u32>(B::nu))};
}
constexpr Table make_table() noexcept {
    Table t{};
    for(u32 a=0;a<rows;++a){t.contextual[a]=contextual(a);if(a==static_cast<u32>(sot))continue;
        for(u32 b=0;b<static_cast<u32>(sot);++b)if(decide(neutral(a),static_cast<B>(b),class_flags(b)).split)t.allowed[a]|=u64{1}<<b;}
    return t;
}
constexpr Table table=make_table();
// Contextual pairs that wait for the next base whatever the context, so the
// scan defers them without the rules: AK|AS × AK|AS (LB28a) and PR|PO × OP
// (LB25), each with its fallback 1.
constexpr bool waits(B a,B b) noexcept {return (in(set(B::ak,B::as),a)&&in(set(B::ak,B::as),b))||(in(affix,a)&&b==B::op);}
constexpr bool waits_agree() noexcept {
    for(u32 a=0;a<static_cast<u32>(sot);++a)for(u32 b=0;b<static_cast<u32>(sot);++b)if(waits(static_cast<B>(a),static_cast<B>(b))){
        const Verdict v=decide(neutral(a),static_cast<B>(b),class_flags(b));
        if(!(table.contextual[a]>>b&1)||v.split!=1||v.next!=(in(affix,static_cast<B>(a))?Defer::prefix:Defer::syllable))return false;}
    return true;
}
static_assert(waits_agree());
// The class LB1 gives a scalar, before LB9 and LB10.
inline B resolved_class(const UnicodeProperties& p) noexcept {
    B b=p.line_break();
    if(in(resolved,b)){const auto category=p.general_category();
        b=b==B::cj?B::ns:b==B::sa&&(category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc)?B::cm:B::al;}
    return b;
}
// The properties of the base at text[at] that the rules read besides its
// class; at is ~0 for sot. A mark there is a base only by LB10 (as U+0041).
u8 flags_at(ConstSlice<u32> text,u32 at,B type) noexcept {
    if(at>=text.count)return 0;
    const u32 cp=text[at];const auto& p=*unicode_properties(cp);const B raw=resolved_class(p);const auto category=p.general_category();
    if(raw==B::cm||raw==B::zwj)return 0;
    u8 flags=p.east_asian_wide()?wide:0;
    if(type==B::qu)flags|=category==UnicodeGeneralCategory::pi?initial:category==UnicodeGeneralCategory::pf?final:0;
    else if(type==B::ak||type==B::as)flags|=aksara;
    else if(cp==0x25cc)flags|=aksara|dotted;
    if(p.has(UnicodeFlag::extended_pictographic)&&category==UnicodeGeneralCategory::cn)flags|=unassigned_pictograph;
    return flags;
}
// The scan keeps each base as its class and position; decide_slow and settle
// read the rest from the text, off the common path.
__declspec(noinline) Verdict decide_slow(ConstSlice<u32> text,Context c,u32 a_at,u32 before_at,B b,u32 at) noexcept {
    c.a.flags=flags_at(text,a_at,c.a.type);c.before.flags=flags_at(text,before_at,c.before.type);
    return decide(c,b,flags_at(text,at,b));
}
// The deferred boundary once the next base b (at text[at]) has arrived, and
// what stays deferred (LB25 waits one more base after OP IS).
struct Settled {u8 value;Defer defer;};
__declspec(noinline) Settled settle(Defer kind,u8 value,B b,ConstSlice<u32> text,u32 at) noexcept {
    if(kind==Defer::final_quote)return {in(after_final,b)?u8{0}:value,Defer::none};
    if(kind==Defer::decimal)return {b==B::nu,Defer::none};
    if(kind==Defer::quote)return {flags_at(text,at,b)&wide?value:u8{0},Defer::none};
    if(kind==Defer::syllable)return {b==B::vf?u8{0}:value,Defer::none};
    if(kind==Defer::prefix&&b==B::is)return {value,Defer::prefix_decimal};
    return {b==B::nu?u8{0}:value,Defer::none};
}
}
bool unicode_line_breaks(ConstSlice<u32> text,Slice<u8> out) noexcept {
    if((text.count&&!text.data)||!out.data||out.count<=text.count)return false;
    if(!text.count){out[0]=0;return true;}
    // One pass with constant state: each boundary is decided from the bases
    // before it, and a deferred one when the bases it reads have arrived.
    B a=sot,before=sot,nonspace=sot;u32 a_at=~0u,before_at=~0u,deferred=0;
    // What the rules carry from boundary to boundary, in one word: the
    // deferred boundary's rule, LB14/LB15a (glue), LB8a (joiner), LB30a
    // parity, held (a rule reads past the pair: LB8, LB14, LB15a, LB20a,
    // LB21a), virama (a is VI after AK, AS or U+25CC: LB28a) and the LB25
    // number state (1: NU (SY|IS)*, 2: then CL or CP).
    constexpr u32 deferral=7,glue=8,joiner=16,ri_odd=32,held=64,virama=128,number_shift=8;
    u32 carry=0;
    for(u32 i=0;i<text.count;++i){const u32 cp=text[i];const UnicodeProperties& p=*unicode_properties(cp);B b=p.line_break();bool zwj=false;
        if(in(resolved|set(B::cm,B::zwj),b)){
            b=resolved_class(p);zwj=b==B::zwj; // LB1
            if(zwj||b==B::cm){
                // LB9: a combining sequence takes its base's class and properties.
                if(!in(hard|set(B::sp,B::zw,sot),a)){out[i]=0;carry=(carry&~joiner)|(zwj?joiner:0);continue;}
                b=B::al; // LB10: as U+0041 (AL, Lu, Na, not pictographic)
            }
        }
        if(carry&deferral){const Defer kind=static_cast<Defer>(carry&deferral);carry&=~deferral;
            if(kind==Defer::syllable){if(b==B::vf)out[deferred]=0;}
            else{const Settled settled=settle(kind,out[deferred],b,text,i);out[deferred]=settled.value;carry|=static_cast<u32>(settled.defer);}}
        // Words and the spaces between them (LB8, LB14, LB15a, LB18, LB28): a
        // letter leaves none of the state the rules carry.
        if(in(letter,b)&&(in(letter,a)||a==B::sp)){out[i]=in(letter,a)||(carry&glue)?0:1;
            carry=zwj?joiner:0;nonspace=b;before=a;before_at=a_at;a=b;a_at=i;continue;}
        // Then the pairs the table answers, LB28a after a virama, and the rules in order.
        const u32 ai=static_cast<u32>(a),bi=static_cast<u32>(b);const bool free=!(carry&(held|joiner|virama));
        if(free&&!((table.contextual[ai]>>bi)&1))out[i]=(table.allowed[ai]>>bi)&1;
        else if(free&&waits(a,b)){out[i]=1;carry|=static_cast<u32>(in(affix,a)?Defer::prefix:Defer::syllable);deferred=i;}
        else if((carry&virama)&&(b==B::ak||(b==B::al&&cp==0x25cc)))out[i]=0;
        else{const Verdict v=decide_slow(text,{{a,0},{before,0},nonspace,(carry&glue)!=0,(carry&joiner)!=0,(carry&ri_odd)!=0,
                static_cast<u8>(carry>>number_shift)},a_at,before_at,b,i);
            out[i]=v.split;if(v.next!=Defer::none){carry|=static_cast<u32>(v.next);deferred=i;}}
        u32 next=(carry&deferral)|(zwj?joiner:0);
        if(!in(stateful,b))nonspace=b;
        else{const u32 number=carry>>number_shift;
            if(b==B::nu||(number==1&&(b==B::sy||b==B::is)))next|=1u<<number_shift;else if(number==1&&(b==B::cl||b==B::cp))next|=2u<<number_shift;
            if(b==B::ri&&!(a==B::ri&&(carry&ri_odd)))next|=ri_odd;
            if(b==B::sp)next|=carry&glue;
            else{if(b==B::op||(b==B::qu&&(flags_at(text,i,b)&initial)&&in(hard|set(B::op,B::qu,B::gl,B::sp,B::zw,sot),a)))next|=glue;nonspace=b;}
            if((next&glue)||nonspace==B::zw||((b==B::hy||b==B::hh)&&in(hyphen_context,a)))next|=held;
            if(b==B::vi&&(a==B::ak||a==B::as||(a==B::al&&text[a_at]==0x25cc)))next|=virama;}
        carry=next;before=a;before_at=a_at;a=b;a_at=i;
    }
    // At eot LB15b and LB19a hold and LB15c fails; the others keep their fallback.
    const Defer last=static_cast<Defer>(carry&deferral);
    if(last==Defer::final_quote||last==Defer::quote||last==Defer::decimal)out[deferred]=0;
    out[text.count]=in(hard,a)?2:1;return true;
}
}
