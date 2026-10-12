#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {

enum class WakeContext : u8 {
    foreground, // focused: deadlines are exact (carets, input feedback)
    background, // visible but unfocused: only long timers may slip
    hidden,     // minimized: nothing visual is due, every timer may slip
};

// TolerableDelay for SetWaitableTimerEx. A tolerant wake lets Windows coalesce
// it with other timer expirations instead of waking the package on its own;
// short visible deadlines (GIF frames, transitions) stay exact.
inline u32 wake_tolerance_ms(u32 timeout_ms, WakeContext context) noexcept {
    if (timeout_ms == kInfiniteWaitMilliseconds || context == WakeContext::foreground)
        return 0;
    if (context == WakeContext::hidden)
        return timeout_ms < 50 ? 0 : minimum(timeout_ms / 4, 1'000u);
    return timeout_ms < 1'000 ? 0 : minimum(timeout_ms / 8, 1'000u);
}

} // namespace maicord
