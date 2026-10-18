#include "maicord/text/font_fallback.hpp"
namespace maicord {namespace {
bool valid(NativeFontProperties p) noexcept {return p.weight&&p.weight<=1000&&p.width&&p.width<=9&&p.style<=2;}
struct Score {i32 distance,dot,x,y,z;u32 simulations,weight;};
Score score(NativeFontProperties p,NativeFontProperties request) noexcept {
    const u32 original=p.weight;u32 simulations=0;
    if(request.style&&!p.style){p.style=1;simulations|=2;}
    if(request.weight>=600&&p.weight<=550&&request.weight>static_cast<u32>(p.weight)+100){p.weight=700;simulations|=1;}
    const i32 qx=(static_cast<i32>(request.width)-5)*1100,qy=request.style*700,qz=(static_cast<i32>(request.weight)-400)*5;
    const i32 x=(static_cast<i32>(p.width)-5)*1100,y=p.style*700,z=(static_cast<i32>(p.weight)-400)*5;
    const i32 dx=x-qx,dy=y-qy,dz=z-qz;
    return {dx*dx+dy*dy+dz*dz,-(x*qx+y*qy+z*qz),absolute_value<i32>(dx),absolute_value<i32>(dy),absolute_value<i32>(dz),simulations,original};
}
bool before(const Score& a,const Score& b) noexcept {
    const i32 left[]{a.distance,a.dot,a.x,a.y,a.z,static_cast<i32>(a.simulations),static_cast<i32>(a.weight)};
    const i32 right[]{b.distance,b.dot,b.x,b.y,b.z,static_cast<i32>(b.simulations),static_cast<i32>(b.weight)};
    for(u32 i=0;i<7;++i)if(left[i]!=right[i])return left[i]<right[i];return false;
}
}
SfntStatus native_font_fallback_order(ConstSlice<NativeFontProperties> candidates,NativeFontProperties requested,
    Slice<u32> output,u32* count,u32 work,u32* used) noexcept {
    const u32 initial=work;struct Report {u32 initial;u32& left;u32* used;~Report() noexcept{if(used)*used=initial-left;}} report{initial,work,used};
    if(count)*count=0;if(!count||!valid(requested)||(candidates.count&&!candidates.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    if(candidates.count>output.count)return SfntStatus::output_too_small;
    for(u32 i=0;i<candidates.count;++i){if(!work)return SfntStatus::resource_limit;--work;if(!valid(candidates[i]))return SfntStatus::invalid_input;
        const auto key=score(candidates[i],requested);u32 at=i;
        while(at){if(!work)return SfntStatus::resource_limit;--work;if(!before(key,score(candidates[output[at-1]],requested)))break;output[at]=output[at-1];--at;}
        output[at]=i;
    }*count=candidates.count;return candidates.count?SfntStatus::ok:SfntStatus::unsupported_sequence;
}
}
