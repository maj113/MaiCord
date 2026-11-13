#include "indic_syllables.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/unicode/unicode.hpp"
namespace maicord {
namespace {
#include "indic_machine.inc"
using C=IndicCategory;using P=IndicPosition;using S=UnicodeIndicSyllabicCategory;using L=UnicodeIndicPositionalCategory;
// IndicSyllabicCategory to syllable category, as HarfBuzz's gen-indic-table.py maps it.
constexpr C syllabic[]{
    C::symbol,C::sm,C::placeholder,C::a,C::c,C::c,C::cm,C::c,C::c,C::m,C::cm,C::placeholder,C::repha,C::x,C::cm,C::cm,C::cs,C::sm,
    C::h,C::zwj,C::x,C::zwnj,C::n,C::placeholder,C::placeholder,C::x,C::m,C::rs,C::x,C::sm,C::x,C::n,C::h,C::sm,C::v,C::m,C::v};
static_assert(sizeof(syllabic)==static_cast<u32>(S::vowel_independent)+1);
// IndicPositionalCategory to position; split forms resolve to their last part.
constexpr P positional[]{
    P::below_c,P::end,P::post_c,P::pre_c,P::post_c,P::end,P::after_main,P::post_c,P::above_c,P::below_c,P::below_c,P::post_c,
    P::above_c,P::post_c,P::post_c,P::pre_m};
static_assert(sizeof(positional)==static_cast<u32>(L::visual_order_left)+1);
// Matra reordering classes of the Microsoft Indic specs per block (HarfBuzz matra_pos_*), indexed
// [block][left,top,bottom,right]; block 9 covers the Vedic and other allowed blocks.
constexpr P matra_positions[10][4]{
    {P::pre_m,P::after_sub,P::after_sub,P::after_sub},{P::pre_m,P::after_sub,P::after_sub,P::after_post},
    {P::pre_m,P::after_post,P::after_post,P::after_post},{P::pre_m,P::after_sub,P::after_post,P::after_post},
    {P::pre_m,P::after_main,P::after_sub,P::after_post},{P::pre_m,P::after_sub,P::after_post,P::after_post},
    {P::pre_m,P::before_sub,P::before_sub,P::before_sub},{P::pre_m,P::before_sub,P::before_sub,P::before_sub},
    {P::pre_m,P::after_sub,P::after_post,P::after_post},{P::pre_m,P::after_sub,P::after_sub,P::after_sub}};
IndicCharacter classify(u32 u,C category,P position) noexcept {
    // Only these categories keep their positional class.
    if(category!=C::cm&&category!=C::sm&&category!=C::rs&&category!=C::h&&category!=C::m&&category!=C::mpst)position=P::end;
    if(indic_consonants&(1u<<static_cast<u32>(category)))position=P::base_c;
    else if(category==C::m||category==C::mpst){
        const u32 block=u>=0x900&&u<0xd80?(u-0x900)>>7:9;
        const u32 side=position==P::pre_c?0:position==P::above_c?1:position==P::below_c?2:3;
        position=matra_positions[block][side];
        // Telugu and Kannada right matras: before the subjoined consonants, except the last ones.
        if(side==3&&block==6)position=u<=0xc42?P::before_sub:P::after_sub;
        if(side==3&&block==7)position=u<0xcc3||u>0xcd6?P::before_sub:P::after_sub;
    }else if(category==C::sm||category==C::smpst||category==C::a||category==C::symbol)position=P::smvd;
    return {category,position};
}
}
IndicCharacter indic_character(u32 u) noexcept {
    // HarfBuzz gen-indic-table.py category and position overrides.
    switch(u){
    case 0x930:case 0x9b0:case 0x9f0:case 0xa30:case 0xab0:case 0xb30:case 0xbb0:case 0xc30:case 0xcb0:case 0xd30:return {C::ra,P::base_c};
    case 0x953:case 0x954:case 0x11301:case 0x11302:case 0x11303:return {C::sm,P::smvd};
    case 0xa40:return {C::mpst,P::after_post};
    case 0xa51:return {C::m,P::below_c};
    case 0xa72:case 0xa73:return {C::c,P::base_c};
    case 0xafb:case 0xb55:case 0x1133b:case 0x1133c:return {C::n,P::end};
    case 0x9fc:case 0xc80:case 0xd04:case 0x2015:case 0x2022:case 0x25fb:case 0x25fc:case 0x25fd:case 0x25fe:return {C::placeholder,P::base_c};
    case 0x25cc:return {C::dotted_circle,P::base_c};
    case 0xb01:return {C::sm,P::before_sub};
    default:break;
    }
    if(u-0x1ce2u<=6u||u==0x1ced)return {C::a,P::smvd};
    if(u-0xa8f2u<=5u||u-0x1ce9u<=3u||u-0x1ceeu<=3u)return {C::symbol,P::smvd};
    // Allowed blocks: Basic Latin, Latin-1, Devanagari..Malayalam, Vedic Extensions, General Punctuation,
    // Superscripts and Subscripts, Devanagari Extended. Myanmar and Khmer characters belong to their own
    // shapers and are plain here.
    if(!(u<0x100||u-0x900u<0x480u||u-0x1cd0u<0x30u||u-0x2000u<0xa0u||u-0xa8e0u<0x20u))return {C::x,P::end};
    const auto& properties=*unicode_properties(u);
    const auto isc=properties.indic_syllabic_category();const auto ipc=properties.indic_positional_category();
    C category=static_cast<u32>(isc)<sizeof(syllabic)?syllabic[static_cast<u32>(isc)]:C::x;
    if(category==C::sm&&ipc==L::na)category=C::smpst;
    return classify(u,category,static_cast<u32>(ipc)<sizeof(positional)?positional[static_cast<u32>(ipc)]:P::end);
}
namespace {
// Canonical decompositions in the Indic blocks, except those HarfBuzz keeps whole (U+0931, U+09DC,
// U+09DD, U+0B94). U+0CCB decomposes through U+0CCA.
struct Decomposition {u16 composite,first,second;};
constexpr Decomposition decompositions[]{
    {0x929,0x928,0x93c},{0x934,0x933,0x93c},{0x958,0x915,0x93c},{0x959,0x916,0x93c},{0x95a,0x917,0x93c},{0x95b,0x91c,0x93c},
    {0x95c,0x921,0x93c},{0x95d,0x922,0x93c},{0x95e,0x92b,0x93c},{0x95f,0x92f,0x93c},{0x9cb,0x9c7,0x9be},{0x9cc,0x9c7,0x9d7},
    {0x9df,0x9af,0x9bc},{0xa33,0xa32,0xa3c},{0xa36,0xa38,0xa3c},{0xa59,0xa16,0xa3c},{0xa5a,0xa17,0xa3c},{0xa5b,0xa1c,0xa3c},
    {0xa5e,0xa2b,0xa3c},{0xb48,0xb47,0xb56},{0xb4b,0xb47,0xb3e},{0xb4c,0xb47,0xb57},{0xb5c,0xb21,0xb3c},{0xb5d,0xb22,0xb3c},
    {0xbca,0xbc6,0xbbe},{0xbcb,0xbc7,0xbbe},{0xbcc,0xbc6,0xbd7},{0xc48,0xc46,0xc56},{0xcc0,0xcbf,0xcd5},{0xcc7,0xcc6,0xcd5},
    {0xcc8,0xcc6,0xcd6},{0xcca,0xcc6,0xcc2},{0xccb,0xcca,0xcd5},{0xd4a,0xd46,0xd3e},{0xd4b,0xd47,0xd3e},{0xd4c,0xd46,0xd57}};
// Block offsets (low seven bits) that some composite uses: most scalars skip the search.
constexpr u64 composite_offset(u32 half) noexcept {
    u64 bits=0;for(const auto& d:decompositions)if(((d.composite>>6)&1u)==half)bits|=1ull<<(d.composite&63u);return bits;
}
constexpr u64 composite_offsets[2]{composite_offset(0),composite_offset(1)};
const Decomposition* find_decomposition(u32 u) noexcept {
    if(u-0x929u>0xd4cu-0x929u||!((composite_offsets[(u>>6)&1u]>>(u&63u))&1u))return nullptr;
    u32 lo=0,hi=sizeof(decompositions)/sizeof(decompositions[0]);
    while(lo<hi){const u32 mid=(lo+hi)/2;if(decompositions[mid].composite<u)lo=mid+1;else hi=mid;}
    return lo<sizeof(decompositions)/sizeof(decompositions[0])&&decompositions[lo].composite==u?decompositions+lo:nullptr;
}
// Canonical compositions whose first part is not a mark (HarfBuzz compose_indic never recomposes split
// matras), plus U+09AF U+09BC, the composition exclusion HarfBuzz recomposes.
u32 composition(u32 a,u32 b) noexcept {
    if(b==0x93c)return a==0x928?0x929u:a==0x930?0x931u:a==0x933?0x934u:0;
    return b==0xbd7&&a==0xb92?0xb94u:b==0x9bc&&a==0x9af?0x9dfu:0;
}
// Combining class with HarfBuzz's modification of the Telugu length marks (84, 91 to 4, 5), which sorts
// them before nukta and virama.
u8 combining(u32 u) noexcept {const u8 value=unicode_properties(u)->combining_class();return value==84?4:value==91?5:value;}
bool is_mark(u32 u) noexcept {
    const auto category=unicode_properties(u)->general_category();
    return category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me;
}
// A mark (any combining class) or a character with a nonzero combining class: it stays in the unit of
// the starter before it, as HarfBuzz normalizes a base with the marks that follow it.
bool joins_unit(u32 u) noexcept {
    const auto& properties=*unicode_properties(u);const auto category=properties.general_category();
    return properties.combining_class()||category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me;
}
// HarfBuzz vowel constraints: a dotted circle between an independent vowel and a dependent sign that
// together would look like another vowel.
bool constrained(SfntIndicProfile profile,u32 before,u32 previous,u32 u) noexcept {
    switch(profile){
    case SfntIndicProfile::devanagari:
        if(before==0x930&&previous==0x94d)return u==0x907;
        switch(previous){
        case 0x905:return u==0x93a||u==0x93b||u==0x93e||u==0x945||u==0x946||u==0x949||u==0x94a||u==0x94b||u==0x94c||u==0x94f||u==0x956||u==0x957;
        case 0x906:return u==0x93a||u-0x945u<=3u;
        case 0x909:return u==0x941;
        case 0x90f:return u-0x945u<=2u;
        default:return false;}
    case SfntIndicProfile::bengali:return (previous==0x985&&u==0x9be)||(previous==0x98b&&u==0x9c3)||(previous==0x98c&&u==0x9e2);
    case SfntIndicProfile::gurmukhi:
        return (previous==0xa05&&(u==0xa3e||u==0xa48||u==0xa4c))||(previous==0xa72&&(u==0xa3f||u==0xa40||u==0xa47))||
            (previous==0xa73&&(u==0xa41||u==0xa42||u==0xa4b));
    case SfntIndicProfile::gujarati:
        return (previous==0xa85&&(u==0xabe||u==0xac5||u==0xac7||u==0xac8||u==0xac9||u==0xacb||u==0xacc))||(previous==0xac5&&u==0xabe);
    case SfntIndicProfile::odia:return (previous==0xb05&&u==0xb3e)||((previous==0xb0f||previous==0xb13)&&u==0xb57);
    case SfntIndicProfile::tamil:return previous==0xb85&&u==0xbc2;
    case SfntIndicProfile::telugu:
        return (previous==0xc12&&(u==0xc4c||u==0xc55))||((previous==0xc3f||previous==0xc46||previous==0xc4a)&&u==0xc55);
    case SfntIndicProfile::kannada:return ((previous==0xc89||previous==0xc8b)&&u==0xcbe)||(previous==0xc92&&u==0xccc);
    case SfntIndicProfile::malayalam:
        return ((previous==0xd07||previous==0xd09)&&u==0xd57)||(previous==0xd0e&&u==0xd46)||(previous==0xd12&&(u==0xd3e||u==0xd57));
    }
    return false;
}
}
SfntStatus IndicReader::mapped(u32 scalar,bool* present) const noexcept {
    if(!cmap_){*present=true;return SfntStatus::ok;}
    u16 glyph=0;const auto status=sfnt_cmap_glyph(*cmap_,scalar,0,&glyph);*present=glyph!=0;return status;
}
// Appends the pieces of one scalar: its decomposition where the font maps every part (HarfBuzz's
// decompose(): the second part must be mapped, the first decomposes further or is mapped), else itself.
SfntStatus IndicReader::decompose(u32 scalar,u32 source,u32* count) noexcept {
    u32 parts[3]{scalar},n=1;
    if(const auto* d=find_decomposition(scalar)){bool second=false;auto status=mapped(d->second,&second);if(status!=SfntStatus::ok)return status;
        if(second){bool first=false;
            if(const auto* inner=find_decomposition(d->first)){bool inner_second=false;status=mapped(inner->second,&inner_second);if(status!=SfntStatus::ok)return status;
                if(inner_second){status=mapped(inner->first,&first);if(status!=SfntStatus::ok)return status;
                    if(first){parts[0]=inner->first;parts[1]=inner->second;parts[2]=d->second;n=3;}}}
            if(n==1){status=mapped(d->first,&first);if(status!=SfntStatus::ok)return status;if(first){parts[0]=d->first;parts[1]=d->second;n=2;}}}}
    for(u32 i=0;i<n;++i)unit_[count_++]=indic_piece(parts[i],source);
    *count=n;return SfntStatus::ok;
}
// Whether the run has no mark after its first character: HarfBuzz then skips reordering and
// recomposition, so decomposed nukta letters stay decomposed.
bool IndicReader::simple() noexcept {
    if(simple_==2){simple_=1;bool first=true;
        for(u32 at=0;at<text_.bytes;){const auto decoded=utf8_decode_one(text_.data+at,text_.bytes-at);if(!decoded.valid)break;
            if(!first&&is_mark(decoded.codepoint)){simple_=0;break;}first=false;at+=decoded.width;}}
    return simple_!=0;
}
// HarfBuzz recomposition inside one unit: a mark joins the last starter when nothing between them has
// an equal or higher combining class and the font maps the composite.
SfntStatus IndicReader::compose() noexcept {
    u32 out=0,starter=0xffffffffu;
    for(u32 i=0;i<count_;++i){const auto piece=unit_[i];const u32 u=piece.scalar();
        if(starter!=0xffffffffu&&is_mark(u)&&(starter==out-1||combining(unit_[out-1].scalar())<combining(u))){
            if(const u32 composed=composition(unit_[starter].scalar(),u)){bool present=false;const auto status=mapped(composed,&present);
                if(status!=SfntStatus::ok)return status;
                if(present&&!simple()){unit_[starter]=indic_piece(composed,unit_[starter].source);continue;}}}
        unit_[out++]=piece;if(!combining(u))starter=out-1;}
    count_=out;return SfntStatus::ok;
}
SfntStatus IndicReader::resume(const IndicReaderMark& mark) noexcept {
    byte_=mark.byte;units_=mark.units;previous_=mark.previous;before_=mark.before;
    const auto status=build();if(status!=SfntStatus::ok)return status;
    read_=mark.read;return SfntStatus::ok;
}
SfntStatus IndicReader::build() noexcept {
    count_=read_=0;unit_byte_=byte_;unit_units_=units_;unit_previous_=previous_;unit_before_=before_;
    if(byte_>=text_.bytes)return SfntStatus::ok;
    u32 marks=0;bool sort=false,composable=false,overflow=false; // marks: length of the current combining run
    for(bool first=true;byte_<text_.bytes;first=false){
        u32 u=ahead_scalar_,width=ahead_width_;
        if(byte_!=ahead_byte_){const auto decoded=utf8_decode_one(text_.data+byte_,text_.bytes-byte_);if(!decoded.valid)return SfntStatus::invalid_input;
            u=decoded.codepoint;width=decoded.width;}
        const bool circle=(previous_||before_)&&constrained(profile_,before_,previous_,u);
        if(!first){
            // A unit ends before the next character that is not a mark (and has a zero combining class)
            // or that receives a vowel-constraint circle; zero-class marks inside stop the sort, as in
            // HarfBuzz, but may still recompose with the starter. A combining run of more than 32 marks
            // is not sorted; its remaining marks follow as continuation units.
            const auto* d=find_decomposition(u);if(circle||!joins_unit(d?d->first:u)){ahead_byte_=byte_;ahead_scalar_=u;ahead_width_=width;break;}
            if(marks==33||count_+3>sizeof(unit_)/sizeof(unit_[0])){overflow=true;break;}}
        if(circle){unit_[count_++]=indic_piece(0x25cc,units_);previous_=before_=0;}else{before_=previous_;previous_=u;}
        u32 n=0;const auto status=decompose(u,units_,&n);if(status!=SfntStatus::ok)return status;
        // Consonant-like pieces have combining class zero; another zero-class piece ends the combining run.
        for(u32 i=count_-n;i<count_;++i){const u32 scalar=unit_[i].scalar();
            if(!((indic_consonants>>static_cast<u32>(unit_[i].category()))&1u)&&combining(scalar))sort|=++marks>1;else marks=0;
            composable|=scalar==0x93c||scalar==0xbd7||scalar==0x9bc;}
        byte_+=width;units_+=u>0xffff?2u:1u;
    }
    // Sort each combining-class run stably unless one is longer than 32 marks or the unit continues one.
    const bool continuation=unit_byte_&&combining(unit_[0].scalar());
    if(sort&&!overflow&&!continuation){
        for(u32 i=1;i<count_;++i){const auto piece=unit_[i];const u8 value=combining(piece.scalar());if(!value)continue;
            u32 at=i;while(at&&combining(unit_[at-1].scalar())>value){unit_[at]=unit_[at-1];--at;}unit_[at]=piece;}}
    if(composable&&!continuation)return compose();
    return SfntStatus::ok;
}
SfntStatus indic_read_syllable(IndicReader& reader,Slice<IndicPiece> out,u32* length,IndicSyllable* outcome) noexcept {
    u32 state=0,count=0;*length=0;*outcome=IndicSyllable::none;IndicReaderMark end{};
    for(;;){IndicPiece piece;bool more=false;const auto status=reader.peek(&piece,&more);if(status!=SfntStatus::ok)return status;
        if(!more)break;
        const u8 next=indic_machine_next[state][static_cast<u8>(piece.category())];if(!next)break;
        if(count==out.count)return SfntStatus::output_too_small;
        out[count++]=piece;reader.take();state=next;
        if(const u8 result=indic_machine_outcome[state]){*length=count;*outcome=static_cast<IndicSyllable>(result);end=reader.mark();}
    }
    return *length<count?reader.resume(end):SfntStatus::ok;
}
}
