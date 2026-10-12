#pragma once
#include "maicord/core/utf.hpp"

namespace maicord {
// The recognized syllable length at first, plus one; negative when a following
// consonant may still be the syllable's final; zero when no syllable starts at first.
i32 native_thai_syllable_result(ConstSlice<u32> text,u32 first) noexcept;
// The syllable length at first including its final consonant and silent
// letters, plus one; zero when none is available.
u32 native_thai_unknown_recovery(ConstSlice<u32> text,u32 first) noexcept;
// False for Thai scalars that attach to the preceding one (dependent vowels,
// LAKKHANGYAO and marks): neither a word nor an unknown fragment ends before them.
bool native_thai_boundary_class(u32 scalar) noexcept;
}
