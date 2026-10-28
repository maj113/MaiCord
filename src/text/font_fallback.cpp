#include "maicord/text/font_fallback.hpp"
#include "sfnt_read.hpp"
namespace maicord {
SfntStatus native_fallback_cascade_find(const NativeFallbackCascade& cascade,u32 scalar,u32* output,u32 work,u32* used) noexcept {
    const sfnt_read::WorkReport report{work,work,used};
    if(output)*output=native_fallback_missing;
    if(!output||scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff))return SfntStatus::invalid_input;
    if(!cascade.ranges.count)return SfntStatus::ok;
    u32 lo=0,hi=cascade.ranges.count;
    while(lo<hi){if(!work)return SfntStatus::resource_limit;--work;const u32 mid=lo+(hi-lo)/2;
        if(cascade.ranges[mid].first<=scalar)lo=mid+1;else hi=mid;}
    *output=lo-1;return SfntStatus::ok;
}
}
