#pragma once
#include "maicord/text/base.hpp"
#include <emmintrin.h>
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
}
