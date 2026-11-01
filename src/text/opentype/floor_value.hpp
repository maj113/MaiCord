#pragma once
#include "maicord/text/base.hpp"

namespace maicord {
// The variation maths rounds with floor(x+.5) (ties toward +infinity), never with
// the CRT, and keeps the sign of a zero result positive. |value| must fit in i64.
// Out of line: one shared copy instead of an expansion at every call site.
__declspec(noinline) inline double floor_value(double value) noexcept {
    const i64 n = static_cast<i64>(value);
    return static_cast<double>(n) - (static_cast<double>(n) > value ? 1 : 0);
}
inline double round_value(double value) noexcept { return floor_value(value + .5); }
} // namespace maicord
