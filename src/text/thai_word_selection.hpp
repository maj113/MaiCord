#pragma once
#include "maicord/core/base.hpp"

namespace maicord {

// Signed lengths distinguish ordinary words from continuation-only candidates.
// The caller supplies lexical lookup; this controller owns no font/dictionary.
struct NativeThaiWordCandidates { i32 lengths[8]{}; };
static_assert(sizeof(NativeThaiWordCandidates)==32);
using NativeThaiWordLookup=NativeThaiWordCandidates(*)(void*,u32) noexcept;

// Retain only the selected next position's candidates. Other second/third
// positions are queried on demand instead of retaining a search tree.
inline u32 native_thai_word_select(u32 position,u32 limit,void* context,NativeThaiWordLookup lookup,
    NativeThaiWordCandidates& retained,NativeThaiWordCandidates& output) noexcept {
    output={};
    if(position>=limit||!lookup){retained={};return 0;}
    const auto magnitude=[](i32 value) noexcept {return value<0?0u-static_cast<u32>(value):static_cast<u32>(value);};
    const auto next=[&](i32 length) noexcept {return position+(length>0?static_cast<u32>(length):0u);};
    const auto roots=retained.lengths[0]?retained:lookup(context,position);
    u32 live=0,total=0,sixth_position=position;
    for(u32 i=0;i<8&&roots.lengths[i];++i){
        const u32 following=next(roots.lengths[i]);
        if(i==5)sixth_position=following;
        if(roots.lengths[i]>0&&following>=limit){output.lengths[0]=roots.lengths[i];retained={};return 1;}
        if(lookup(context,following).lengths[0]){live|=1u<<i;++total;}
    }
    if(!total){output.lengths[0]=roots.lengths[0];retained={};return 0;}
    const bool unique=total==1;
    u32 chosen=8,written=0;
    for(u32 i=0;i<8;++i){
        if(!roots.lengths[i])break;
        const i32 first=roots.lengths[i];
        if(unique){
            if(!(live&(1u<<i)))continue;
            chosen=i;output.lengths[written++]=first;break;
        }
        u32 following=i==5?sixth_position:next(first);
        const auto seconds=lookup(context,following);
        if(chosen==8&&!seconds.lengths[0])continue;
        for(u32 j=0;j<8;++j){
            // Segmentation reuses the sixth successor slot for third-word lookahead.
            // That observable choice is kept with just a cursor; immutable lookup
            // reconstructs the slot without a stored tree.
            if(i==5)following=sixth_position;
            const i32 candidate=i==5?lookup(context,following).lengths[j]:seconds.lengths[j];
            if(!candidate)break;
            const u32 second=magnitude(candidate),third_at=following+second;
            if(chosen==8){
                if(third_at<limit&&roots.lengths[5])sixth_position=third_at;
                if(third_at>=limit||lookup(context,third_at).lengths[0]){
                    chosen=i;output.lengths[written++]=first;break;
                }
            }else{
                const u32 selected=magnitude(output.lengths[0]),pair=magnitude(first)+second;
                if(selected==pair)break;
                if(third_at>=limit)continue;
                if(roots.lengths[5])sixth_position=third_at;
                const auto thirds=lookup(context,third_at);
                if(thirds.lengths[0]){
                    if(selected!=pair+magnitude(thirds.lengths[0]))output.lengths[written++]=first;
                    break;
                }
            }
        }
        if(!--total)break;
    }
    if(chosen==8){output={};output.lengths[0]=roots.lengths[0];retained={};return 0;}
    retained=output.lengths[0]>0?lookup(context,chosen==5?sixth_position:next(roots.lengths[chosen])):NativeThaiWordCandidates{};
    u32 positive=0;
    for(u32 i=0;i<written;++i)if(output.lengths[i]>0)output.lengths[positive++]=output.lengths[i];
    for(u32 i=positive;i<8;++i)output.lengths[i]=0;
    return positive;
}
}
