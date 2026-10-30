#include "maicord/text/font_match.hpp"
namespace maicord {namespace {
// Lexicographic key: projected distance, negative dot product, per-axis
// distances, simulation key and weight key. Validated domains bound squared
// distance below 105 million and the dot product below 31 million, so signed
// 32-bit arithmetic is sufficient. Matching keys the simulation count and the
// distance of the original weight; fallback ordering keys the simulation flags
// and the ascending original weight.
struct Rank {i32 key[7];NativeFontProperties properties;u32 simulations;};
Rank rank(NativeFontProperties face,NativeFontProperties request,bool ordering) noexcept {
    NativeFontProperties p=face;u32 simulations=0;
    if(request.style&&!p.style){p.style=1;simulations|=2;}
    if(request.weight>=600&&p.weight<=550&&request.weight>static_cast<u32>(p.weight)+100){p.weight=700;simulations|=1;}
    const i32 qx=(static_cast<i32>(request.width)-5)*1100,qy=request.style*700,qz=(static_cast<i32>(request.weight)-400)*5;
    const i32 x=(static_cast<i32>(p.width)-5)*1100,y=p.style*700,z=(static_cast<i32>(p.weight)-400)*5;
    const i32 dx=x-qx,dy=y-qy,dz=z-qz;
    return {{dx*dx+dy*dy+dz*dz,-(x*qx+y*qy+z*qz),absolute_value<i32>(dx),absolute_value<i32>(dy),absolute_value<i32>(dz),
        ordering?static_cast<i32>(simulations):static_cast<i32>((simulations&1)+(simulations>>1)),
        ordering?static_cast<i32>(face.weight):absolute_value<i32>(static_cast<i32>(face.weight)-request.weight)},p,simulations};
}
bool before(const Rank& a,const Rank& b) noexcept {
    for(u32 i=0;i<7;++i)if(a.key[i]!=b.key[i])return a.key[i]<b.key[i];
    return false;
}
}
NativeFontMatch native_font_match(ConstSlice<NativeFontProperties> candidates,NativeFontProperties requested) noexcept {
    Rank best=rank(candidates[0],requested,false);u32 index=0;
    for(u32 i=1;i<candidates.count;++i){const Rank next=rank(candidates[i],requested,false);if(before(next,best)){best=next;index=i;}}
    return {index,best.properties,best.simulations};
}
void native_font_fallback_order(ConstSlice<NativeFontProperties> candidates,NativeFontProperties requested,u32* order) noexcept {
    for(u32 i=0;i<candidates.count;++i){const Rank key=rank(candidates[i],requested,true);u32 at=i;
        for(;at&&before(key,rank(candidates[order[at-1]],requested,true));--at)order[at]=order[at-1];
        order[at]=i;}
}
}
