#pragma once
// Single-precision helpers shared by the line and paragraph fitters. add rounds
// each sum to float (no contraction or wider intermediate), so every fitter
// accumulates advances identically.
#include "maicord/text/base.hpp"
#include <emmintrin.h>
namespace maicord {
inline float layout_add(float a,float b) noexcept {return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a),_mm_set_ss(b)));}
inline bool layout_finite(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);return (bits&0x7f800000u)!=0x7f800000u;}
}
