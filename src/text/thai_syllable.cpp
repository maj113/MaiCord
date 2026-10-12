#include "thai_syllable.hpp"

namespace maicord {namespace {
#include "generated/thai_syllable_data.inc"

u32 syllable_scalar(ConstSlice<u32> text,u32 first,u32 offset) noexcept {
    return offset<text.count-first?text[first+offset]:0;
}
bool syllable_leading(u32 cp) noexcept {return cp-0xe40u<5;}
bool syllable_consonant(u32 cp) noexcept {return cp-0xe01u<46;}
bool syllable_odd(u32 cp) noexcept {
    const u32 index=cp-0xe00u;
    return index>=96||(thai_syllable_odd[index>>3]&(1u<<(index&7)))!=0;
}
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
bool native_thai_boundary_class(u32 scalar) noexcept {return syllable_odd(scalar);}
u32 native_thai_unknown_recovery(ConstSlice<u32> text,u32 first) noexcept {
    if(!text.data||first>=text.count)return 0;
    if(text.count-first>13){
        for(u32 i=10;i;--i)if(text[first+i]==0xe4c)
            return i+2+(syllable_consonant(text[first+i+1])&&syllable_leading(text[first+i+2])?1u:0u);
    }
    const i32 raw=native_thai_syllable_result(text,first);
    if(raw>=0)return static_cast<u32>(raw);
    const u32 length=static_cast<u32>(-raw),previous=syllable_scalar(text,first,length-1);
    if(syllable_leading(previous))return length;
    if(!syllable_consonant(previous))return 0;
    const u32 following=syllable_scalar(text,first,length);
    if(!syllable_odd(following))return length;
    return syllable_leading(following)?length+1:0;
}
}
