#include "maicord/core/byte_range.hpp"

namespace maicord {

bool byte_ranges_overlap(const void* first, usize first_bytes, const void* second, usize second_bytes) noexcept {
    if (!first_bytes || !second_bytes)
        return false;
    const usize a = reinterpret_cast<usize>(first), b = reinterpret_cast<usize>(second);
    return a <= b ? b - a < first_bytes : a - b < second_bytes;
}

bool byte_ranges_overlap_or_wrap(const void* first, usize first_bytes, const void* second,
                                 usize second_bytes) noexcept {
    const usize a = reinterpret_cast<usize>(first), b = reinterpret_cast<usize>(second);
    return a > ~usize{0} - first_bytes || b > ~usize{0} - second_bytes ||
           (first_bytes && second_bytes && a < b + second_bytes && b < a + first_bytes);
}

bool byte_ranges_conflict(const void* first, usize first_bytes, const void* second, usize second_bytes) noexcept {
    return first_bytes && second_bytes &&
           (!first || !second || byte_ranges_overlap_or_wrap(first, first_bytes, second, second_bytes));
}

} // namespace maicord
