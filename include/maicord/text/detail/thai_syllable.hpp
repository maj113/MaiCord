#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/base.hpp"

namespace maicord {
// The recognized syllable length at first, plus one; negative when a following
// consonant may still be the syllable's final; zero when no syllable starts at first.
i32 native_thai_syllable_result(ConstSlice<u32> text,u32 first) noexcept;
// The syllable length at first including its final consonant and silent
// letters, plus one; zero when none is available. syllable is
// native_thai_syllable_result(text,first).
u32 native_thai_unknown_recovery(ConstSlice<u32> text,u32 first,i32 syllable) noexcept;
inline u32 native_thai_unknown_recovery(ConstSlice<u32> text,u32 first) noexcept {
    return native_thai_unknown_recovery(text,first,native_thai_syllable_result(text,first));
}
// False for Thai scalars that attach to the preceding one (dependent vowels,
// LAKKHANGYAO and marks): neither a word nor an unknown fragment ends before them.
bool native_thai_boundary_class(u32 scalar) noexcept;
}
