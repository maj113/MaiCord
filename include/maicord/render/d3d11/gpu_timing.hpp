#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

inline constexpr u64 kGpuTimestampScale = 1'000'000ull;

inline u64 gpu_timestamp_microseconds(u64 ticks, u64 frequency) noexcept {
    if (frequency == 0 || frequency > ~0ull / kGpuTimestampScale) {
        return 0;
    }
    const u64 whole_seconds = ticks / frequency;
    const u64 fraction = ((ticks % frequency) * kGpuTimestampScale) / frequency;
    if (whole_seconds > (~0ull - fraction) / kGpuTimestampScale) {
        return ~0ull;
    }
    return whole_seconds * kGpuTimestampScale + fraction;
}

} // namespace maicord
