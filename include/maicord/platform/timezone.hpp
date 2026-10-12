#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kIanaTimezoneMaximumBytes = 64;
// Queries the Windows ICU component once at account startup. Returns zero
// when unavailable/unknown or when the output cannot hold the whole ID + NUL.
u32 host_iana_timezone(Slice<char> output) noexcept;
} // namespace maicord
