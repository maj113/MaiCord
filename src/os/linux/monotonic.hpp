#pragma once

#include "maicord/sys/linux/abi.hpp"
#include "maicord/sys/linux/libc.hpp"

// The deadline arithmetic of the Linux os layer: waits are timed on CLOCK_MONOTONIC (D19), which is also the clock
// futex and ppoll relative timeouts run on.

namespace maicord {

inline i64 monotonic_nanoseconds() noexcept {
    TimeSpec time{};
    clock_gettime(kClockMonotonic, &time);
    return time.seconds * 1'000'000'000 + time.nanoseconds;
}

inline TimeSpec timespec_from_nanoseconds(i64 nanoseconds) noexcept {
    if (nanoseconds < 0) {
        nanoseconds = 0;
    }
    return TimeSpec{nanoseconds / 1'000'000'000, nanoseconds % 1'000'000'000};
}

} // namespace maicord
