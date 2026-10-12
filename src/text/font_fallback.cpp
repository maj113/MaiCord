#include "maicord/text/font_fallback.hpp"
namespace maicord {
SfntStatus native_fallback_cascade_find(const NativeFallbackCascade& cascade,u32 scalar,u32* output,u32 work,u32* used) noexcept {
    const u32 initial=work;struct Report {u32 initial;u32& left;u32* out;~Report() noexcept{if(out)*out=initial-left;}} report{initial,work,used};
    if(output)*output=native_fallback_missing;
    if(!output||scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff))return SfntStatus::invalid_input;
    if(!cascade.ranges.count)return SfntStatus::ok;
    u32 lo=0,hi=cascade.ranges.count;
    while(lo<hi){if(!work)return SfntStatus::resource_limit;--work;const u32 mid=lo+(hi-lo)/2;
        if(cascade.ranges[mid].first<=scalar)lo=mid+1;else hi=mid;}
    *output=lo-1;return SfntStatus::ok;
}
SfntStatus native_fallback_cascade_select(const NativeFallbackCascade& cascade,u32 scalar,NativeFallbackCoverage coverage,void* context,
    u32* output,u32 work,u32* used) noexcept {
    const u32 initial=work;struct Report {u32 initial;u32& left;u32* out;~Report() noexcept{if(out)*out=initial-left;}} report{initial,work,used};
    if(output)*output=native_fallback_missing;
    if(!output||!coverage||scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff))return SfntStatus::invalid_input;
    u32 index=native_fallback_missing,scanned=0;const auto found=native_fallback_cascade_find(cascade,scalar,&index,work,&scanned);work-=scanned;
    if(found!=SfntStatus::ok)return found;if(index==native_fallback_missing)return SfntStatus::ok;
    const auto& r=cascade.ranges[index];
    for(u32 i=0;i<r.count;++i){if(!work)return SfntStatus::resource_limit;--work;
        const u32 token=cascade.tokens[r.offset+i];bool covered=false;const auto status=coverage(context,token,scalar,&covered);
        if(status!=SfntStatus::ok)return status;if(covered){*output=token;return SfntStatus::ok;}}
    return SfntStatus::ok;
}
}
