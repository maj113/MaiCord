#pragma once

#include "maicord/ui/text/measure_cache.hpp"

namespace maicord {

// Counts the measurements text measure caches ask of their backends (their misses). The first call for a cache puts
// a counting backend in front of the cache's own one. The counting backend passes the cache's context through
// unchanged, so a test may still move the cache to another renderer by its context. Initializing the cache again
// removes the counter; the next call starts it again at zero.
struct CountedTextMeasure {
    const UiTextMeasureCache* cache;
    UiTextMeasureBackend backend;
    u32 calls;
};

inline CountedTextMeasure g_counted_text_measure[8];

template <u32 Slot>
bool counted_text_measure(void* context, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    ++g_counted_text_measure[Slot].calls;
    return g_counted_text_measure[Slot].backend(context, request, value);
}

inline constexpr UiTextMeasureBackend kCountedTextMeasure[8]{
    counted_text_measure<0>, counted_text_measure<1>, counted_text_measure<2>, counted_text_measure<3>,
    counted_text_measure<4>, counted_text_measure<5>, counted_text_measure<6>, counted_text_measure<7>};

// The backend calls of `cache` since the first call for it; ~0u once 8 cache addresses count.
inline u32 text_measure_calls(UiTextMeasureCache* cache) noexcept {
    for (u32 slot = 0; slot < 8; ++slot)
        if (cache->backend == kCountedTextMeasure[slot])
            return g_counted_text_measure[slot].calls;
    // A cache at an address seen before was initialized again: its slot starts over.
    for (u32 slot = 0; slot < 8; ++slot) {
        auto& counter = g_counted_text_measure[slot];
        if (counter.cache && counter.cache != cache)
            continue;
        counter = {cache, cache->backend, 0};
        cache->backend = kCountedTextMeasure[slot];
        return 0;
    }
    return ~0u;
}

} // namespace maicord
