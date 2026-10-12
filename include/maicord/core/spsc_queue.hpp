#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

// One producer and one consumer. Monotonic 32-bit sequences make every slot
// usable; unsigned subtraction remains correct across wrap as long as Capacity
// is far below 2^31. Interlocked publication provides the cross-core fence.
template <typename T, u32 Capacity> struct SpscQueue {
    static_assert(Capacity >= 2);
    static_assert((Capacity & (Capacity - 1)) == 0);
    static_assert(Capacity < 0x80000000u);

    T items[Capacity];
    volatile LONG read_sequence;
    u8 read_padding[60];
    volatile LONG write_sequence;
    u8 write_padding[60];
    volatile LONG dropped;
};

template <typename T, u32 Capacity>
bool spsc_queue_push(SpscQueue<T, Capacity>* queue, const T& value) noexcept {
    if (!queue) {
        return false;
    }
    const u32 write =
        static_cast<u32>(_InterlockedCompareExchange(&queue->write_sequence, 0, 0));
    const u32 read =
        static_cast<u32>(_InterlockedCompareExchange(&queue->read_sequence, 0, 0));
    if (write - read >= Capacity) {
        _InterlockedIncrement(&queue->dropped);
        return false;
    }
    queue->items[write & (Capacity - 1)] = value;
    _InterlockedExchange(&queue->write_sequence, static_cast<LONG>(write + 1));
    return true;
}

template <typename T, u32 Capacity>
bool spsc_queue_pop(SpscQueue<T, Capacity>* queue, T* output) noexcept {
    if (!queue || !output) {
        return false;
    }
    const u32 read =
        static_cast<u32>(_InterlockedCompareExchange(&queue->read_sequence, 0, 0));
    const u32 write =
        static_cast<u32>(_InterlockedCompareExchange(&queue->write_sequence, 0, 0));
    if (read == write) {
        return false;
    }
    *output = queue->items[read & (Capacity - 1)];
    _InterlockedExchange(&queue->read_sequence, static_cast<LONG>(read + 1));
    return true;
}

template <typename T, u32 Capacity>
u32 spsc_queue_size(const SpscQueue<T, Capacity>& queue) noexcept {
    const u32 write = static_cast<u32>(_InterlockedCompareExchange(
        const_cast<volatile LONG*>(&queue.write_sequence), 0, 0));
    const u32 read = static_cast<u32>(_InterlockedCompareExchange(
        const_cast<volatile LONG*>(&queue.read_sequence), 0, 0));
    return write - read;
}

template <typename T, u32 Capacity>
u32 spsc_queue_dropped(const SpscQueue<T, Capacity>& queue) noexcept {
    return static_cast<u32>(_InterlockedCompareExchange(
        const_cast<volatile LONG*>(&queue.dropped), 0, 0));
}

} // namespace maicord
