#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Presence-only repaints share one trailing window: the first change arms the
// deadline, later changes join it, and taking the flush disarms it.
inline constexpr u64 kPresenceCoalesceMilliseconds = 150;

struct PresenceCoalescer {
    u64 flush_at_ms;
    u32 noted;
    u32 flushes;
};

inline void presence_coalescer_note(PresenceCoalescer* coalescer, u64 now_ms) noexcept {
    saturating_increment(coalescer->noted);
    if (coalescer->flush_at_ms == 0)
        coalescer->flush_at_ms = now_ms + kPresenceCoalesceMilliseconds;
}

inline bool presence_coalescer_take(PresenceCoalescer* coalescer, u64 now_ms) noexcept {
    if (coalescer->flush_at_ms == 0 || now_ms < coalescer->flush_at_ms)
        return false;
    coalescer->flush_at_ms = 0;
    saturating_increment(coalescer->flushes);
    return true;
}

} // namespace maicord
