#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline bool byte_range_valid(ConstSlice<u8> span) noexcept {
    return span.count == 0 || (span.data && reinterpret_cast<usize>(span.data) <= ~usize{0} - span.count);
}
inline bool byte_ranges_overlap(ConstSlice<u8> a, ConstSlice<u8> b) noexcept {
    if (!a.count || !b.count)
        return false;
    const usize begin_a = reinterpret_cast<usize>(a.data);
    const usize begin_b = reinterpret_cast<usize>(b.data);
    return begin_a <= begin_b ? begin_b - begin_a < a.count : begin_a - begin_b < b.count;
}
} // namespace maicord
