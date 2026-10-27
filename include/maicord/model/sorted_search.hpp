#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// First position in [0, count) whose element does not order before the key;
// before(position) reports whether the element there orders before it. The
// elements must be sorted by the same order.
template <typename Before>
constexpr u32 sorted_lower_bound(u32 count, Before before) noexcept {
    u32 first = 0;
    while (count != 0) {
        const u32 step = count / 2;
        if (before(first + step)) {
            first += step + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

} // namespace maicord
