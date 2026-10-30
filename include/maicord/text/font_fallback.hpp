#pragma once
#include "maicord/text/sfnt.hpp"
namespace maicord {
// Ordered scalar routing, separate from family matching, availability and layout
// itemization. No platform fallback table is embedded here. A caller prepares an
// appropriate policy and binds its opaque tokens to native family/coverage data.
struct NativeFallbackRange {u32 first;u16 offset,count;};
struct NativeFallbackCascade {ConstSlice<NativeFallbackRange> ranges;ConstSlice<u16> tokens;u32 token_count;};
constexpr u32 native_fallback_missing=0xffffffffu;
// Callback sets covered on success. An unavailable family is successful false;
// malformed font data, IO and capacity failures must remain errors, not misses.
// The callback manages any lazily loaded font mapping/cache. No callback output is kept.
using NativeFallbackCoverage=SfntStatus(*)(void* context,u32 token,u32 scalar,bool* covered) noexcept;
// Validate once, then keep the input arrays alive and unchanged. Nonempty ranges
// start at zero and increase strictly through Unicode scalar space; each extends
// to the next first (or 0x110000). Empty ranges are an empty policy. List storage
// may be shared/overlapping. All tokens are less than token_count (at most 65536).
// Output is disjoint from inputs. Failure clears output; no allocation or OS call.
constexpr SfntStatus native_fallback_cascade_open(ConstSlice<NativeFallbackRange> ranges,ConstSlice<u16> tokens,u32 token_count,
    NativeFallbackCascade* output,u32 maximum_items=1000000) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if((ranges.count&&!ranges.data)||(tokens.count&&!tokens.data)||token_count>65536)return SfntStatus::invalid_input;
    if(ranges.count>maximum_items||tokens.count>maximum_items-ranges.count)return SfntStatus::resource_limit;
    for(u32 i=0;i<tokens.count;++i)if(tokens[i]>=token_count)return SfntStatus::invalid_input;
    for(u32 i=0;i<ranges.count;++i){const auto& r=ranges[i];
        if((!i&&r.first)||(i&&r.first<=ranges[i-1].first)||r.first>0x10ffff||
           r.offset>tokens.count||r.count>tokens.count-r.offset)return SfntStatus::invalid_input;}
    *output={ranges,tokens,token_count};return SfntStatus::ok;
}
// The interval holding a valid Unicode scalar, independently of coverage. Its
// index is a stable routing ID for this cascade, even when adjacent intervals
// share a token list. Bind that initial list before mapping a text prefix. An
// empty policy returns native_fallback_missing.
inline u32 native_fallback_cascade_find(const NativeFallbackCascade& cascade,u32 scalar) noexcept {
    u32 lo=0,hi=cascade.ranges.count;
    while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(cascade.ranges[mid].first<=scalar)lo=mid+1;else hi=mid;}
    return lo-1;
}
static_assert(sizeof(NativeFallbackRange)==8&&sizeof(NativeFallbackCascade)==40);
}
