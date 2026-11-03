#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/memory.hpp"

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

// Opens a gap at `at` of a sorted order array of `count` elements (room for one more) by moving
// [at, count) one place up: one move instead of an element loop.
template <typename T> inline void sorted_insert_gap(T* elements, u32 count, u32 at) noexcept {
    if (count > at)
        copy_bytes(elements + at + 1, elements + at, (count - at) * static_cast<u32>(sizeof(T)));
}

// Closes the gap at `at` of an array of `count` elements by moving (at, count) one place down: one move
// instead of an element loop.
template <typename T> inline void sorted_erase_gap(T* elements, u32 count, u32 at) noexcept {
    copy_bytes(elements + at, elements + at + 1, (count - at - 1) * static_cast<u32>(sizeof(T)));
}

// Moves the element at `from` to `to`, shifting the ones between by one place: one move instead of an element loop.
template <typename T> inline void sorted_move_gap(T* elements, u32 from, u32 to) noexcept {
    const T moving = elements[from];
    if (from < to)
        copy_bytes(elements + from, elements + from + 1, (to - from) * static_cast<u32>(sizeof(T)));
    else if (to < from)
        copy_bytes(elements + to + 1, elements + to, (from - to) * static_cast<u32>(sizeof(T)));
    elements[to] = moving;
}
} // namespace maicord
