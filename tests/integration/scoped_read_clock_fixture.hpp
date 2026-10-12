#pragma once

// Target-only forced include: load the actual inline clock definitions before
// redirecting controller call sites. Linked libraries retain their real clocks.
#include "maicord/core/time.hpp"

namespace maicord {
u64 scoped_native_clock_monotonic_milliseconds() noexcept;
u64 scoped_native_clock_unix_milliseconds() noexcept;
} // namespace maicord

#define clock_monotonic_milliseconds scoped_native_clock_monotonic_milliseconds
#define clock_unix_milliseconds scoped_native_clock_unix_milliseconds
