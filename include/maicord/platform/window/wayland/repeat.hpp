#pragma once

#include "maicord/core/base.hpp"

// Client-side key repeat of the Wayland backend (Linux). The compositor sends one press and one release and leaves
// repeating to the client, with the rate and delay of wl_keyboard.repeat_info. The state holds the last repeating
// key and the time of its next repeat; time is passed in (milliseconds on any monotonic clock), so tests inject it.
// At most one repeat fires per key_repeat_fire call even when the loop was late, as Windows coalesces repeats; the
// next one stays on the original cadence.

namespace maicord::wayland {

inline constexpr u32 kRepeatNever = 0xFFFFFFFFu;

struct KeyRepeat {
    bool armed;
    u32 token;       // the evdev code of the repeating key
    u64 due_ms;
    u32 interval_ms;
};

// Starts repeating `token` after `delay_ms`, then `rate_per_second` times a second. A rate of 0 (repeat disabled
// by the compositor) leaves it stopped. Replaces any key that was repeating, as a new key press does on Windows.
inline void key_repeat_start(KeyRepeat* repeat, u32 token, u64 now_ms, u32 delay_ms, u32 rate_per_second) noexcept {
    repeat->armed = rate_per_second != 0;
    repeat->token = token;
    repeat->due_ms = now_ms + delay_ms;
    repeat->interval_ms = rate_per_second > 1000 ? 1 : rate_per_second ? 1000 / rate_per_second : 1;
}

inline void key_repeat_stop(KeyRepeat* repeat) noexcept {
    repeat->armed = false;
}

// Stops only when `token` is the repeating key (its release).
inline void key_repeat_release(KeyRepeat* repeat, u32 token) noexcept {
    if (repeat->armed && repeat->token == token)
        repeat->armed = false;
}

// Milliseconds until the next repeat (0 when it is due), kRepeatNever when nothing repeats.
inline u32 key_repeat_wait_ms(const KeyRepeat& repeat, u64 now_ms) noexcept {
    if (!repeat.armed)
        return kRepeatNever;
    if (repeat.due_ms <= now_ms)
        return 0;
    const u64 wait = repeat.due_ms - now_ms;
    return wait >= kRepeatNever ? kRepeatNever - 1 : static_cast<u32>(wait);
}

// True when one repeat is due at `now_ms`; the next is scheduled on the original cadence, after `now_ms`.
inline bool key_repeat_fire(KeyRepeat* repeat, u64 now_ms) noexcept {
    if (!repeat->armed || repeat->due_ms > now_ms)
        return false;
    const u64 late = now_ms - repeat->due_ms;
    repeat->due_ms += (late / repeat->interval_ms + 1) * repeat->interval_ms;
    return true;
}

} // namespace maicord::wayland
