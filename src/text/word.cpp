#include "maicord/text/word.hpp"

namespace maicord {
namespace {
using W=UnicodeWordBreak;
constexpr u8 pictograph=0x40;
W type(u8 tag) noexcept {return static_cast<W>(tag&0x1f);}
bool newline(W value) noexcept {return value==W::cr || value==W::lf || value==W::nl;}
bool ignored(W value) noexcept {return value==W::extend || value==W::fo || value==W::zwj;}
bool letter(W value) noexcept {return value==W::le || value==W::hl;}
bool middle_letter(W value) noexcept {return value==W::ml || value==W::mb || value==W::sq;}
bool middle_number(W value) noexcept {return value==W::mn || value==W::mb || value==W::sq;}
bool extended_word(W value) noexcept {return letter(value) || value==W::nu || value==W::ka || value==W::ex;}
}
bool unicode_word_boundaries(ConstSlice<u32> scalars,Slice<u8> boundaries) noexcept {
    if((scalars.count && !scalars.data) || !boundaries.data || boundaries.count<=scalars.count)return false;
    for(u32 i=0;i<scalars.count;++i)if(!unicode_is_scalar(scalars[i]))return false;
    if(!scalars.count){boundaries[0]=0;return true;}
    for(u32 i=0;i<scalars.count;++i) {
        const auto& properties=*unicode_properties(scalars[i]);
        boundaries[i]=static_cast<u8>(properties.word_break()) |
                      (properties.has(UnicodeFlag::extended_pictographic)?pictograph:0);
    }
    W raw_previous=W::xx,previous=W::xx,before_previous=W::xx;bool ri_odd=false;
    for(u32 i=0;i<scalars.count;++i) {
        const u8 tag=boundaries[i];const W current=type(tag);bool split=true;
        if(!i)split=true; // WB1
        else if(raw_previous==W::cr && current==W::lf)split=false; // WB3
        else if(newline(raw_previous) || newline(current))split=true; // WB3a/b
        else if(raw_previous==W::zwj && (tag&pictograph))split=false; // WB3c
        else if(raw_previous==W::wsegspace && current==W::wsegspace)split=false; // WB3d
        else if(ignored(current))split=false; // WB4
        else {
            // Only a significant punctuation position needs lookahead. Ignored
            // spans following different punctuation positions are disjoint, so
            // this forward scan has a linear total bound without a next array.
            W following=W::xx;
            if((letter(previous) && middle_letter(current)) ||
               (previous==W::hl && current==W::dq) ||
               (previous==W::nu && middle_number(current))) {
                u32 next=i+1;while(next<scalars.count && ignored(type(boundaries[next])))++next;
                if(next<scalars.count)following=type(boundaries[next]);
            }
            if(letter(previous) && letter(current))split=false; // WB5
            else if(letter(previous) && middle_letter(current) && letter(following))split=false; // WB6
            else if(letter(before_previous) && middle_letter(previous) && letter(current))split=false; // WB7
            else if(previous==W::hl && current==W::sq)split=false; // WB7a
            else if(previous==W::hl && current==W::dq && following==W::hl)split=false; // WB7b
            else if(before_previous==W::hl && previous==W::dq && current==W::hl)split=false; // WB7c
            else if(previous==W::nu && current==W::nu)split=false; // WB8
            else if(letter(previous) && current==W::nu)split=false; // WB9
            else if(previous==W::nu && letter(current))split=false; // WB10
            else if(before_previous==W::nu && middle_number(previous) && current==W::nu)split=false; // WB11
            else if(previous==W::nu && middle_number(current) && following==W::nu)split=false; // WB12
            else if(previous==W::ka && current==W::ka)split=false; // WB13
            else if(extended_word(previous) && current==W::ex)split=false; // WB13a
            else if(previous==W::ex && (letter(current) || current==W::nu || current==W::ka))split=false; // WB13b
            else if(previous==W::ri && current==W::ri && ri_odd)split=false; // WB15/16
        }
        boundaries[i]=split?1:0;raw_previous=current;
        if(!ignored(current)) {
            before_previous=previous;previous=current;
            ri_odd=current==W::ri?!ri_odd:false;
        }
    }
    boundaries[scalars.count]=1; // WB2
    return true;
}
}
