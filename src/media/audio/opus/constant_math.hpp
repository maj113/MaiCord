#pragma once
// Sine and cosine for tables built at compile time (double precision, error below 1e-16).
#include "maicord/core/base.hpp"

namespace maicord::opus {

inline constexpr double kPi = 3.14159265358979323846;

// sin(angle) for |angle| <= pi / 4 when `cosine` is false, else cos(angle).
constexpr double constant_series(double angle, bool cosine) noexcept {
    const double square = angle * angle;
    double term = cosine ? 1.0 : angle, sum = term;
    for (i32 k = cosine ? 1 : 2; k < 24; k += 2) {
        term *= -square / (k * (k + 1));
        sum += term;
    }
    return sum;
}
constexpr double constant_sine(double angle) noexcept {
    // Quadrant reduction: angle = quarter * pi / 2 + rest, |rest| <= pi / 4.
    const double scaled = angle * (2.0 / kPi);
    const i64 quarter = static_cast<i64>(scaled + (scaled >= 0 ? 0.5 : -0.5));
    const double rest = angle - static_cast<double>(quarter) * (kPi / 2.0);
    switch (quarter & 3) {
    case 0:
        return constant_series(rest, false);
    case 1:
        return constant_series(rest, true);
    case 2:
        return -constant_series(rest, false);
    default:
        return -constant_series(rest, true);
    }
}
constexpr double constant_cosine(double angle) noexcept {
    return constant_sine(angle + kPi / 2.0);
}

} // namespace maicord::opus
