#pragma once
#include "maicord/text/base.hpp"
#include "maicord/sys/simd.hpp"
namespace maicord::tt_math {
#pragma float_control(precise,on,push)
inline void normalized_vector(i64 x,i64 y,i16* vx,i16* vy) noexcept {
    if(!x&&!y){*vx=16384;*vy=0;return;}
    const double dx=static_cast<double>(x),dy=static_cast<double>(y);
    const double length=_mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(),_mm_set_sd(dx*dx+dy*dy)));
    const double scale=16384.0/length,nx=dx*scale,ny=dy*scale;
    *vx=static_cast<i16>(nx+(nx<0?-.5:.5));*vy=static_cast<i16>(ny+(ny<0?-.5:.5));
}
#pragma float_control(pop)
inline i64 floor_div(i64 n,i64 d) noexcept {return n<0?-((-n+d-1)/d):n/d;}
// a*b/denominator rounded to nearest (ties away from zero); false on a zero denominator or overflow.
inline bool multiply_divide(i64 a,i64 b,i64 denominator,i64* out) noexcept {
    if(!denominator)return false;
    const u64 x=static_cast<u64>(absolute_value<i64>(a)),y=static_cast<u64>(absolute_value<i64>(b)),d=static_cast<u64>(absolute_value<i64>(denominator));
    if(y&&x>0xffffffffffffffffull/y)return false;
    const u64 product=x*y,q=product/d,round=product%d>=(d+1)/2?1:0;
    if(q>0x7fffffffffffffffull-round)return false;
    *out=((a<0)^(b<0)^(denominator<0))?-static_cast<i64>(q+round):static_cast<i64>(q+round);return true;
}
}
