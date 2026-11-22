#pragma once
// Four- and eight-lane float vectors for the codec's kernels: the compiler's vector types, whose operators
// and __builtin_shufflevector become SSE and AVX instructions. Sums and products keep their written order, and a
// multiply-add fuses only through an fmadd intrinsic or fused() in a MAICORD_HASWELL_FMA function, so results are the
// same on every build.
#include "maicord/core/base.hpp"
#include "maicord/sys/simd_haswell.hpp"

namespace maicord::opus {

typedef float F4 __attribute__((__vector_size__(16)));
typedef float F8 __attribute__((__vector_size__(32)));
typedef i32 I4 __attribute__((__vector_size__(16)));
typedef u32 U4 __attribute__((__vector_size__(16))); // wrapping arithmetic
// For loads and stores at any address of the element type.
typedef float F4Unaligned __attribute__((__vector_size__(16), __aligned__(4), __may_alias__));
typedef float F8Unaligned __attribute__((__vector_size__(32), __aligned__(4), __may_alias__));
typedef u32 U4Unaligned __attribute__((__vector_size__(16), __aligned__(4), __may_alias__));

__forceinline F4 load4(const float* from) noexcept {
    return *reinterpret_cast<const F4Unaligned*>(from);
}
__forceinline F8 load8(const float* from) noexcept {
    return *reinterpret_cast<const F8Unaligned*>(from);
}
__forceinline void store4(float* to, F4 value) noexcept {
    *reinterpret_cast<F4Unaligned*>(to) = value;
}
__forceinline void store8(float* to, F8 value) noexcept {
    *reinterpret_cast<F8Unaligned*>(to) = value;
}
__forceinline F4 splat4(float value) noexcept {
    return F4{value, value, value, value};
}
__forceinline F8 splat8(float value) noexcept {
    return F8{value, value, value, value, value, value, value, value};
}
__forceinline F8 reverse8(F8 value) noexcept {
    return __builtin_shufflevector(value, value, 7, 6, 5, 4, 3, 2, 1, 0);
}
__forceinline F4 reverse4(F4 value) noexcept {
    return __builtin_shufflevector(value, value, 3, 2, 1, 0);
}
__forceinline F4 low4(F8 value) noexcept {
    return __builtin_shufflevector(value, value, 0, 1, 2, 3);
}
__forceinline F4 high4(F8 value) noexcept {
    return __builtin_shufflevector(value, value, 4, 5, 6, 7);
}
// ((v0 + v2) + (v1 + v3)), and for eight lanes the two halves added first.
__forceinline float sum4(F4 value) noexcept {
    const F4 pairs = value + __builtin_shufflevector(value, value, 2, 3, 2, 3);
    return pairs[0] + pairs[1];
}
__forceinline float sum8(F8 value) noexcept {
    return sum4(low4(value) + high4(value));
}
// a * b + c, rounded once.
MAICORD_HASWELL_FMA __forceinline float fused(float a, float b, float c) noexcept {
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
}

// Integer lanes for the SILK encoder's fixed point.
template <typename To, typename From> __forceinline To as(From value) noexcept {
    return __builtin_bit_cast(To, value);
}
__forceinline I4 splat(i32 value) noexcept {
    return I4{value, value, value, value};
}
// multiply_16(a, b) in each lane, for a b whose high halves are zero.
__forceinline I4 multiply_low(I4 a, I4 b) noexcept {
    return as<I4>(_mm_madd_epi16(as<__m128i>(a), as<__m128i>(b)));
}
// v negated where s is negative, zero where s is zero.
__forceinline I4 signed_as(I4 v, I4 s) noexcept {
    return as<I4>(_mm_sign_epi32(as<__m128i>(v), as<__m128i>(s)));
}
// The first lane where v equals m; one lane must.
__forceinline u32 first_equal(I4 v, I4 m) noexcept {
    return static_cast<u32>(__builtin_ctz(static_cast<u32>(_mm_movemask_epi8(as<__m128i>(v == m))))) >> 2;
}
// The first lane holding the least (greatest) value, and that value in every lane.
struct Extreme {
    u32 at;
    I4 value;
};
__forceinline Extreme least(I4 v) noexcept {
    I4 m = __builtin_elementwise_min(v, __builtin_shufflevector(v, v, 2, 3, 0, 1));
    m = __builtin_elementwise_min(m, __builtin_shufflevector(m, m, 1, 0, 3, 2));
    return {first_equal(v, m), m};
}
__forceinline Extreme greatest(I4 v) noexcept {
    I4 m = __builtin_elementwise_max(v, __builtin_shufflevector(v, v, 2, 3, 0, 1));
    m = __builtin_elementwise_max(m, __builtin_shufflevector(m, m, 1, 0, 3, 2));
    return {first_equal(v, m), m};
}

} // namespace maicord::opus
