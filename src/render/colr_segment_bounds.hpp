#pragma once
#include "maicord/text/outline.hpp"
#include <emmintrin.h>
#include <stddef.h>

namespace maicord::colr_detail {
static_assert(offsetof(GlyphOutline,minimum_y)==offsetof(GlyphOutline,minimum_x)+4 &&
    offsetof(GlyphOutline,maximum_x)==offsetof(GlyphOutline,minimum_x)+8 &&
    offsetof(GlyphOutline,maximum_y)==offsetof(GlyphOutline,minimum_x)+12);
// The caller supplies count complete 16-byte (ax,ay,bx,by) segments.
// Preserve point order and MIN's second-operand tie/NaN behavior. Negating
// the upper pair lets one packed minimum also maintain both maxima.
inline void segment_bounds(const void* segments, u32 count, GlyphOutline& output) noexcept {
    output.has_bounds = count != 0;
    if (!count) return;
    const auto* bytes = static_cast<const u8*>(segments);
    const auto sign = _mm_castsi128_ps(_mm_set_epi32(-2147483647-1, -2147483647-1, 0, 0));
    const auto* end = bytes + static_cast<usize>(count)*16;
    auto pair = _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes)));
    auto bounds = _mm_xor_ps(_mm_shuffle_ps(pair, pair, _MM_SHUFFLE(1,0,1,0)), sign);
    bytes += 8;
    do {
        pair = _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes)));
        bounds = _mm_min_ps(bounds, _mm_xor_ps(_mm_shuffle_ps(pair, pair, _MM_SHUFFLE(1,0,1,0)), sign));
        bytes += 8;
    } while (bytes != end);
    _mm_storeu_ps(&output.minimum_x, _mm_xor_ps(bounds, sign));
}
}
