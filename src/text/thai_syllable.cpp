#include "thai_syllable.hpp"

namespace maicord {namespace {
// First-party grammar; see tools/unicode/thai_syllable_data.py.
#include "generated/thai_syllable_data.inc"

u32 syllable_scalar(ConstSlice<u32> text,u32 first,u32 offset) noexcept {
    return offset<text.count-first?text[first+offset]:0;
}
bool syllable_consonant(u32 cp) noexcept {return cp-0xe01u<46;}
}
i32 native_thai_syllable_result(ConstSlice<u32> text,u32 first) noexcept {
    if(!text.data||first>=text.count)return 0;
    u32 state=0,position=0;
    for(;;){
        const u32 index=syllable_scalar(text,first,position)-0xe00u;
        const u32 token=index<92?thai_syllable_classes[index]:thai_syllable_other;
        const u8* row=thai_syllable_edges+thai_syllable_rows[state];
        u32 target=row[0];
        for(u32 i=0;i<row[1];++i)if(row[2+i*2]==token){target=row[3+i*2];break;}
        if(target<251){state=target;++position;continue;}
        if(target==255)return 0;
        if(target==254)return static_cast<i32>(position+2);
        if(target==251)return static_cast<i32>(position+1);
        return -static_cast<i32>(position+(target==253?2u:1u));
    }
}
// Dependent vowels, LAKKHANGYAO, tone marks and the other combining marks
// attach to the preceding scalar; everything else can begin a cluster.
bool native_thai_boundary_class(u32 scalar) noexcept {
    return !(scalar-0xe30u<11||scalar==0xe45||scalar-0xe47u<8);
}
u32 native_thai_unknown_recovery(ConstSlice<u32> text,u32 first) noexcept {
    if(!text.data||first>=text.count)return 0;
    const auto at=[&](u32 offset) noexcept {return syllable_scalar(text,first,offset);};
    const i32 raw=native_thai_syllable_result(text,first);
    if(!raw)return 0;
    u32 length=static_cast<u32>(raw>0?raw:-raw)-1;
    // After an open syllable a consonant is its final unless marks attach to
    // the consonant, which then begins the next syllable.
    if(raw<0&&syllable_consonant(at(length))&&native_thai_boundary_class(at(length+1)))++length;
    // Marks attached to the last scalar, and a consonant silenced by
    // THANTHAKHAT one or two scalars later (as in จันทร์), end the syllable.
    // A run of stray marks stops at 31 scalars.
    while(length<31&&!native_thai_boundary_class(at(length)))++length;
    if(syllable_consonant(at(length))&&(at(length+1)==0xe4c||at(length+2)==0xe4c)){
        length+=at(length+1)==0xe4c?2u:3u;
        while(length<31&&!native_thai_boundary_class(at(length)))++length;
    }
    return length+1;
}
}
