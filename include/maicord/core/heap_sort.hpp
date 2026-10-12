#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

template <typename Value, typename Less>
void heap_sort_sift_down(Value* values, u32 root, u32 count,
                         const Less& less) noexcept {
    for (;;) {
        const u32 left = root * 2u + 1u;
        if (left >= count) {
            return;
        }
        u32 greater = left;
        const u32 right = left + 1u;
        if (right < count && less(values[left], values[right])) {
            greater = right;
        }
        if (!less(values[root], values[greater])) {
            return;
        }
        const Value temporary = values[root];
        values[root] = values[greater];
        values[greater] = temporary;
        root = greater;
    }
}

template <typename Value, typename Less>
void heap_sort(Value* values, u32 count, const Less& less) noexcept {
    for (u32 root = count / 2; root != 0; --root) {
        heap_sort_sift_down(values, root - 1, count, less);
    }
    for (u32 end = count; end > 1; --end) {
        const Value temporary = values[0];
        values[0] = values[end - 1];
        values[end - 1] = temporary;
        heap_sort_sift_down(values, 0, end - 1, less);
    }
}

} // namespace maicord
