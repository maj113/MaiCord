#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// An unstable in-place O(n log n) sort with O(1) extra space. Two forms with the same comparison order:
//   heap_sort         inlines the comparator into its own copy of the sort; for per-event and per-frame paths.
//   heap_sort_shared  one out-of-line body serves every element type and comparator, at one indirect call per
//                     comparison (two to three times the time of heap_sort, measured on 100 to 200,000 small
//                     elements); for sorts that run when a list opens or a request completes.

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

// `less(context, left, right)` on two elements of the sorted array.
using HeapSortLess = bool (*)(const void* context, const void* left, const void* right) noexcept;

// Exchanges two `size`-byte elements.
__forceinline void heap_sort_exchange(u8* left, u8* right, u32 size) noexcept {
    if (size == 8) {
        u64 a, b;
        __builtin_memcpy(&a, left, 8);
        __builtin_memcpy(&b, right, 8);
        __builtin_memcpy(left, &b, 8);
        __builtin_memcpy(right, &a, 8);
        return;
    }
    if (size == 4) {
        u32 a, b;
        __builtin_memcpy(&a, left, 4);
        __builtin_memcpy(&b, right, 4);
        __builtin_memcpy(left, &b, 4);
        __builtin_memcpy(right, &a, 4);
        return;
    }
    for (; size >= 8; size -= 8, left += 8, right += 8) {
        u64 a, b;
        __builtin_memcpy(&a, left, 8);
        __builtin_memcpy(&b, right, 8);
        __builtin_memcpy(left, &b, 8);
        __builtin_memcpy(right, &a, 8);
    }
    for (; size; --size, ++left, ++right) {
        const u8 a = *left;
        *left = *right;
        *right = a;
    }
}

// The heap is built, then its root is moved behind the shrinking heap; one loop and one sift serve both phases.
__declspec(noinline) inline void heap_sort_core(void* data, u32 count, u32 size, HeapSortLess less,
                                                const void* context) noexcept {
    u8* const values = static_cast<u8*>(data);
    const usize stride = size;
    u32 root = count / 2, end = count;
    while (end > 1) {
        if (root)
            --root;
        else {
            --end;
            heap_sort_exchange(values, values + end * stride, size);
        }
        for (u32 at = root;;) {
            const u32 left = at * 2u + 1u;
            if (left >= end)
                break;
            u32 greater = left;
            if (left + 1u < end && less(context, values + left * stride, values + (left + 1u) * stride))
                greater = left + 1u;
            if (!less(context, values + at * stride, values + greater * stride))
                break;
            heap_sort_exchange(values + at * stride, values + greater * stride, size);
            at = greater;
        }
    }
}

template <typename Value, typename Less>
void heap_sort_shared(Value* values, u32 count, const Less& less) noexcept {
    static_assert(is_trivially_copyable_v<Value>);
    heap_sort_core(
        values, count, sizeof(Value),
        [](const void* context, const void* left, const void* right) noexcept -> bool {
            return (*static_cast<const Less*>(context))(*static_cast<const Value*>(left),
                                                         *static_cast<const Value*>(right));
        },
        &less);
}

} // namespace maicord
