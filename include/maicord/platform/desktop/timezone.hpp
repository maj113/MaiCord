#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kIanaTimezoneMaximumBytes = 64;
// The IANA identifier of the user's time zone, queried once at account startup: the Windows ICU component
// (src/platform/desktop/timezone.cpp), or on Linux the zone os/local_time.hpp loads ($TZ, /etc/localtime, /etc/timezone).
// Returns zero when unavailable/unknown or when the output cannot hold the whole ID + NUL.
u32 host_iana_timezone(Slice<char> output) noexcept;
} // namespace maicord
