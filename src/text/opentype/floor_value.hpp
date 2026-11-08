#pragma once
#include "maicord/text/base.hpp"

namespace maicord {
// The variation maths rounds with floor(x+.5) (ties toward +infinity), never with
// the CRT, and keeps the sign of a zero result positive (the + 0). |value| must
// fit in i64. The floor is one SSE4.1 roundsd, which the AVX baseline includes.
inline double floor_value(double value) noexcept { return __builtin_elementwise_floor(value) + 0.0; }
inline double round_value(double value) noexcept { return floor_value(value + .5); }
} // namespace maicord
