#pragma once
// Four- and eight-lane float vectors for the codec's kernels: the compiler's vector types, whose operators
// and
// __builtin_shufflevector become SSE and AVX instructions (the baseline has no FMA and no 256-bit
// integers). Sums and products keep their written order, so results are the same on every build.
#include "maicord/core/base.hpp"

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
__forceinline F8 join8(F4 low, F4 high) noexcept {
    return __builtin_shufflevector(low, high, 0, 1, 2, 3, 4, 5, 6, 7);
}
// ((v0 + v2) + (v1 + v3)), and for eight lanes the two halves added first.
__forceinline float sum4(F4 value) noexcept {
    const F4 pairs = value + __builtin_shufflevector(value, value, 2, 3, 2, 3);
    return pairs[0] + pairs[1];
}
__forceinline float sum8(F8 value) noexcept {
    return sum4(low4(value) + high4(value));
}

} // namespace maicord::opus
