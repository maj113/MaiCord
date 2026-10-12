#pragma once
#include "maicord/core/utf.hpp"

namespace maicord {
// Internal primitives for bounded Thai/space/ZWSP spans; results are relative
// to first. The syllable result is the recognized length plus one, negative
// when a following consonant may still be the syllable's final, or zero when
// no syllable starts at first. Recovery returns the length of the syllable,
// with its final consonant and silent letters, plus one, or zero when none is
// available. Neither primitive decides when the word breaker calls it.
i32 native_thai_syllable_result(ConstSlice<u32> text,u32 first) noexcept;
u32 native_thai_unknown_recovery(ConstSlice<u32> text,u32 first) noexcept;
// False for Thai scalars that attach to the preceding one (dependent vowels,
// LAKKHANGYAO and marks): neither a word nor an unknown fragment ends before them.
bool native_thai_boundary_class(u32 scalar) noexcept;
}
