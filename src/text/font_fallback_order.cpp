#include "maicord/text/font_fallback.hpp"
#include "sfnt_read.hpp"
namespace maicord {namespace {
NativeFontRank score(NativeFontProperties p,NativeFontProperties request) noexcept {
    const u32 original=p.weight;u32 simulations=0;
    if(request.style&&!p.style){p.style=1;simulations|=2;}
    if(request.weight>=600&&p.weight<=550&&request.weight>static_cast<u32>(p.weight)+100){p.weight=700;simulations|=1;}
    const i32 qx=(static_cast<i32>(request.width)-5)*1100,qy=request.style*700,qz=(static_cast<i32>(request.weight)-400)*5;
    const i32 x=(static_cast<i32>(p.width)-5)*1100,y=p.style*700,z=(static_cast<i32>(p.weight)-400)*5;
    const i32 dx=x-qx,dy=y-qy,dz=z-qz;
    return {dx*dx+dy*dy+dz*dz,-(x*qx+y*qy+z*qz),absolute_value<i32>(dx),absolute_value<i32>(dy),absolute_value<i32>(dz),static_cast<i32>(simulations),static_cast<i32>(original)};
}
}
SfntStatus native_font_fallback_order(ConstSlice<NativeFontProperties> candidates,NativeFontProperties requested,
    Slice<u32> output,u32* count,u32 work,u32* used) noexcept {
    const sfnt_read::WorkReport report{work,work,used};
    if(count)*count=0;if(!count||!native_font_properties_valid(requested)||(candidates.count&&!candidates.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    if(candidates.count>output.count)return SfntStatus::output_too_small;
    for(u32 i=0;i<candidates.count;++i){if(!work)return SfntStatus::resource_limit;--work;if(!native_font_properties_valid(candidates[i]))return SfntStatus::invalid_input;
        const auto key=score(candidates[i],requested);u32 at=i;
        while(at){if(!work)return SfntStatus::resource_limit;--work;if(!native_font_rank_before(key,score(candidates[output[at-1]],requested)))break;output[at]=output[at-1];--at;}
        output[at]=i;
    }*count=candidates.count;return candidates.count?SfntStatus::ok:SfntStatus::unsupported_sequence;
}
}
