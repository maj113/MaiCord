#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

// One producer and one consumer. Monotonic 32-bit sequences make every slot
// usable; unsigned subtraction remains correct across wrap as long as Capacity
// is far below 2^31. Acquire loads and release stores publish each item, as in
// SpscMailbox; the build pins x64 /volatile:ms semantics. Publication is not a
// full fence: a caller that checks for a waiting consumer after a push must
// order that check itself.
template <typename T, u32 Capacity> struct SpscQueue {
    static_assert(Capacity >= 2);
    static_assert((Capacity & (Capacity - 1)) == 0);
    static_assert(Capacity < 0x80000000u);

    T items[Capacity];
    volatile LONG read_sequence;
    u8 read_padding[60];
    volatile LONG write_sequence;
    u8 write_padding[60];
};

template <typename T, u32 Capacity>
bool spsc_queue_push(SpscQueue<T, Capacity>* queue, const T& value) noexcept {
    if (!queue) {
        return false;
    }
    const u32 write = static_cast<u32>(read_acquire(&queue->write_sequence));
    const u32 read = static_cast<u32>(read_acquire(&queue->read_sequence));
    if (write - read >= Capacity) {
        return false;
    }
    queue->items[write & (Capacity - 1)] = value;
    write_release(&queue->write_sequence, static_cast<LONG>(write + 1));
    return true;
}

template <typename T, u32 Capacity>
bool spsc_queue_pop(SpscQueue<T, Capacity>* queue, T* output) noexcept {
    if (!queue || !output) {
        return false;
    }
    const u32 read = static_cast<u32>(read_acquire(&queue->read_sequence));
    const u32 write = static_cast<u32>(read_acquire(&queue->write_sequence));
    if (read == write) {
        return false;
    }
    *output = queue->items[read & (Capacity - 1)];
    write_release(&queue->read_sequence, static_cast<LONG>(read + 1));
    return true;
}

template <typename T, u32 Capacity>
u32 spsc_queue_size(const SpscQueue<T, Capacity>& queue) noexcept {
    const u32 write = static_cast<u32>(read_acquire(&queue.write_sequence));
    const u32 read = static_cast<u32>(read_acquire(&queue.read_sequence));
    return write - read;
}

} // namespace maicord
