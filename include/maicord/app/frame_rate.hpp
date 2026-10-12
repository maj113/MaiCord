#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Count successful presents, not paint requests or animation/worker ticks.
// One final zero sample stops the deadline while the renderer is idle.
struct FrameRateCounter {
    u64 window_start_us;
    u64 next_update_us;
    u32 frames;
    u32 fps;
};

inline void frame_rate_presented(FrameRateCounter& counter, u64 now_us) noexcept {
    if (counter.next_update_us == 0) {
        counter.window_start_us = now_us;
        counter.next_update_us = now_us + 500'000;
    }
    ++counter.frames;
}

inline bool frame_rate_update(FrameRateCounter& counter, u64 now_us) noexcept {
    if (counter.next_update_us == 0 || now_us < counter.next_update_us) return false;
    const u64 elapsed = now_us - counter.window_start_us;
    const u32 fps = elapsed != 0
        ? static_cast<u32>((static_cast<u64>(counter.frames) * 1'000'000 + elapsed / 2) / elapsed)
        : 0;
    const bool changed = fps != counter.fps;
    counter.fps = fps;
    counter.next_update_us = counter.frames != 0 ? now_us + 500'000 : 0;
    counter.window_start_us = now_us;
    counter.frames = 0;
    return changed;
}

} // namespace maicord
