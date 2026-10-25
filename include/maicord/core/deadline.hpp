#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// INFINITE: a Win32 wait that never times out.
inline constexpr u32 kInfiniteWaitMilliseconds = 0xFFFFFFFFu;
inline constexpr u32 kFiniteWaitMaximumMilliseconds = 0xFFFFFFFEu;

constexpr u64 deadline_after(u64 now, u64 delay) noexcept {
    const u64 sum = now + delay;
    return sum < now ? ~0ull : sum;
}

constexpr u64 earliest_deadline(u64 first, u64 second) noexcept {
    if (first == 0) {
        return second;
    }
    if (second == 0) {
        return first;
    }
    return minimum(first, second);
}

constexpr bool deadline_due(u64 now, u64 deadline) noexcept {
    return deadline != 0 && now >= deadline;
}

constexpr u32 wait_milliseconds_until(u64 now, u64 deadline,
                                      u32 maximum_wait = kFiniteWaitMaximumMilliseconds) noexcept {
    if (deadline <= now) {
        return 0;
    }
    const u64 remaining = deadline - now;
    return remaining > maximum_wait ? maximum_wait : static_cast<u32>(remaining);
}

} // namespace maicord
