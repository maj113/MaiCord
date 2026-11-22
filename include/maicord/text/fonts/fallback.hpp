#pragma once
#include "maicord/text/opentype/sfnt.hpp"
namespace maicord {
// Ordered scalar routing, separate from family matching, availability and layout
// itemization. No platform fallback table is embedded here. A caller prepares an
// appropriate policy and binds its opaque tokens to native family/coverage data.
// A range is its first scalar and its token list. The list packs its offset (below 2048) and its count (below 32)
// into 16 bits, so a range takes 6 bytes and the search reads only the first scalars.
constexpr u16 native_fallback_list(u32 offset,u32 count) noexcept {return static_cast<u16>(offset<<5|count);}
constexpr u32 native_fallback_list_offset(u16 list) noexcept {return list>>5u;}
constexpr u32 native_fallback_list_count(u16 list) noexcept {return list&31u;}
struct NativeFallbackCascade {ConstSlice<u32> firsts;ConstSlice<u16> lists;ConstSlice<u16> tokens;u32 token_count;};
constexpr u32 native_fallback_missing=0xffffffffu;
// Callback sets covered on success. An unavailable family is successful false;
// malformed font data, IO and capacity failures must remain errors, not misses.
// The callback manages any lazily loaded font mapping/cache. No callback output is kept.
using NativeFallbackCoverage=SfntStatus(*)(void* context,u32 token,u32 scalar,bool* covered) noexcept;
// Validate once (the generated policies do it at compile time), then keep the input arrays alive and unchanged.
// firsts and lists hold one entry for each range. Nonempty ranges start at zero and increase strictly
// through Unicode scalar space; each extends to the next first (or 0x110000). List storage may be shared/overlapping.
// All tokens are less than token_count (at most 65536). Output is disjoint from inputs. Failure clears output.
constexpr SfntStatus native_fallback_cascade_open(ConstSlice<u32> firsts,ConstSlice<u16> lists,ConstSlice<u16> tokens,u32 token_count,
    NativeFallbackCascade* output) noexcept {
    *output={};
    for(u32 i=0;i<tokens.count;++i)if(tokens[i]>=token_count)return SfntStatus::invalid_input;
    for(u32 i=0;i<firsts.count;++i){const u32 first=firsts[i],offset=native_fallback_list_offset(lists[i]);
        if((!i&&first)||(i&&first<=firsts[i-1])||first>0x10ffff||
           offset>tokens.count||native_fallback_list_count(lists[i])>tokens.count-offset)return SfntStatus::invalid_input;}
    *output={firsts,lists,tokens,token_count};return SfntStatus::ok;
}
// The interval holding a valid Unicode scalar, independently of coverage. Its
// index is a stable routing ID for this cascade, even when adjacent intervals
// share a token list. Bind that initial list before mapping a text prefix. An
// empty policy returns native_fallback_missing.
inline u32 native_fallback_cascade_find(const NativeFallbackCascade& cascade,u32 scalar) noexcept {
    u32 lo=0,hi=cascade.firsts.count;
    while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(cascade.firsts[mid]<=scalar)lo=mid+1;else hi=mid;}
    return lo-1;
}
static_assert(sizeof(NativeFallbackCascade)==56);
}
