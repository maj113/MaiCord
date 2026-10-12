#include "maicord/text/font_match.hpp"
namespace maicord {namespace {
bool valid(NativeFontProperties p) noexcept {return p.weight&&p.weight<=1000&&p.width&&p.width<=9&&p.style<=2;}
i32 absolute(i32 n) noexcept {return n<0?-n:n;}
struct Rank {i32 distance,negative_dot,x,y,z,simulations,base_weight;};
bool before(const Rank& a,const Rank& b) noexcept {
    const i32 left[]{a.distance,a.negative_dot,a.x,a.y,a.z,a.simulations,a.base_weight};
    const i32 right[]{b.distance,b.negative_dot,b.x,b.y,b.z,b.simulations,b.base_weight};
    for(u32 i=0;i<7;++i)if(left[i]!=right[i])return left[i]<right[i];return false;
}
}
SfntStatus native_font_match(ConstSlice<NativeFontProperties> candidates,NativeFontProperties requested,NativeFontMatch* output,u32 maximum_candidates) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!valid(requested)||(candidates.count&&!candidates.data))return SfntStatus::invalid_input;
    if(candidates.count>maximum_candidates)return SfntStatus::resource_limit;
    if(!candidates.count)return SfntStatus::unsupported_sequence;
    const i32 qx=(static_cast<i32>(requested.width)-5)*1100,qy=requested.style*700,qz=(static_cast<i32>(requested.weight)-400)*5;
    NativeFontMatch result{};Rank best{};
    for(u32 i=0;i<candidates.count;++i){const auto base=candidates[i];if(!valid(base))return SfntStatus::invalid_input;
        auto p=base;u32 simulations=0;
        if(requested.style&&p.style==0){p.style=1;simulations|=2;}
        if(requested.weight>=600&&p.weight<=550&&requested.weight>static_cast<u32>(p.weight)+100){p.weight=700;simulations|=1;}
        const i32 x=(static_cast<i32>(p.width)-5)*1100,y=p.style*700,z=(static_cast<i32>(p.weight)-400)*5;
        const i32 dx=x-qx,dy=y-qy,dz=z-qz;
        // Validated domains bound squared distance below 105 million and the
        // dot product below 31 million; signed 32-bit arithmetic is sufficient.
        const Rank rank{dx*dx+dy*dy+dz*dz,-(x*qx+y*qy+z*qz),absolute(dx),absolute(dy),absolute(dz),
            static_cast<i32>((simulations&1)+((simulations>>1)&1)),absolute(static_cast<i32>(base.weight)-requested.weight)};
        if(!i||before(rank,best)){best=rank;result={i,p,simulations};}
    }
    *output=result;return SfntStatus::ok;
}
}
