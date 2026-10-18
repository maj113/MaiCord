#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline bool byte_range_valid(const void* data, usize bytes) noexcept {
    return bytes == 0 || (data && reinterpret_cast<usize>(data) <= ~usize{0} - bytes);
}
inline bool byte_range_valid(ConstSlice<u8> span) noexcept {
    return byte_range_valid(span.data, span.count);
}
// Empty ranges overlap nothing.
bool byte_ranges_overlap(const void* first, usize first_bytes, const void* second, usize second_bytes) noexcept;
inline bool byte_ranges_overlap(ConstSlice<u8> a, ConstSlice<u8> b) noexcept {
    return byte_ranges_overlap(a.data, a.count, b.data, b.count);
}
// Both ranges are valid and do not overlap.
inline bool byte_ranges_disjoint(ConstSlice<u8> a, ConstSlice<u8> b) noexcept {
    return byte_range_valid(a) && byte_range_valid(b) && !byte_ranges_overlap(a, b);
}
// Every range is valid and none overlaps another.
inline bool byte_ranges_all_disjoint(ConstSlice<ConstSlice<u8>> ranges) noexcept {
    for (u32 i = 0; i < ranges.count; ++i) {
        if (!byte_range_valid(ranges.data[i]))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (byte_ranges_overlap(ranges.data[i], ranges.data[j]))
                return false;
    }
    return true;
}
// Also true when either range runs past the end of the address space; an
// empty range never does.
bool byte_ranges_overlap_or_wrap(const void* first, usize first_bytes, const void* second,
                                 usize second_bytes) noexcept;
// Two nonempty ranges that cannot be used together: either is null or runs
// past the end of the address space, or they overlap. False when either is empty.
bool byte_ranges_conflict(const void* first, usize first_bytes, const void* second, usize second_bytes) noexcept;
} // namespace maicord
